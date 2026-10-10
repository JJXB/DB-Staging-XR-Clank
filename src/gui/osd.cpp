// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// In-headset HUD (see osd.h): VR menu, OSD toasts, virtual keyboard,
// controller ray ribbons, desktop-OSD compositing, and XR preset
// (xrcfg) save/load.

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
#include "gui/xrneural.h"
#include "dos/programs/xrcfg.h"
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
SectionProp* get_osd_section()
{
	auto section = get_section("osd");
	assert(section);
	return section;
}

void init_osd_config_settings(SectionProp& section)
{
	using enum Property::Changeable::Value;

	auto pbool_desk = section.AddBool("osd_desk", Always, true);
	pbool_desk->SetHelp(
	        "Draw notification toasts in the desktop mirror window ('on'\n"
	        "by default). Independent from the in-headset overlay, so the\n"
	        "window can stay quiet while the headset shows messages and\n"
	        "vice versa. Messages fade after a few seconds.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");

	auto pint_desk_opacity = section.AddInt("osd_desk_opacity",
	                                        Always,
	                                        100);
	pint_desk_opacity->SetHelp(
	        "Opacity of the notification toasts drawn in the desktop\n"
	        "mirror window as a percentage ('100' by default, meaning\n"
	        "opaque toasts like the headset shows). Lower values fade\n"
	        "the toasts over the desktop picture; 0 hides them entirely.\n"
	        "Same convention as osd_xr_opacity. Valid range from 0\n"
	        "to 100.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_desk_opacity->SetMinMax(0, 100);

	auto pint_desk_scale = section.AddInt("osd_desk_scale",
	                                      Always,
	                                      100);
	pint_desk_scale->SetHelp(
	        "Size of the notification toasts drawn in the desktop mirror\n"
	        "window as a percentage of their default size ('100' by\n"
	        "default, meaning two-thirds of the frame width). The toast\n"
	        "fonts scale with the panel. Shrunk to fit if it would\n"
	        "exceed the frame. Valid range from 25 to 200.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_desk_scale->SetMinMax(25, 200);

	auto pbool_xr = section.AddBool("osd_xr", Always, true);
	pbool_xr->SetHelp(
	        "Show an in-headset notification overlay for XR toggles and\n"
	        "setting changes ('on' by default). Independent from the\n"
	        "desktop-window toasts. Messages fade after a few\n"
	        "seconds.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");

	auto pint_xr_dist = section.AddInt("osd_xr_distance", Always, 200);
	pint_xr_dist->SetHelp(
	        "Distance of the in-headset notification overlay in\n"
	        "centimeters ('200' by default). Independent from the screen\n"
	        "distance. Valid range from 50 to 500.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_dist->SetMinMax(50, 500);

	auto pint_xr_scale = section.AddInt("osd_xr_scale", Always, 100);
	pint_xr_scale->SetHelp(
	        "Size of the in-headset notification overlay as a percentage of\n"
	        "its default size ('100' by default). Scaled about its centre\n"
	        "above the DOS picture. Valid range from 25 to 200.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_scale->SetMinMax(25, 200);

	auto pint_xr_opacity = section.AddInt("osd_xr_opacity",
	                                      Always,
	                                      100);
	pint_xr_opacity->SetHelp(
	        "Opacity of the in-headset notification overlay as a\n"
	        "percentage ('100' by default, meaning opaque). Lower values\n"
	        "fade the toasts over the scene behind them. Valid range\n"
	        "from 10 to 100.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_opacity->SetMinMax(10, 100);

	auto pint_xr_x = section.AddInt("osd_xr_offset_x", Always, 0);
	pint_xr_x->SetHelp(
	        "Horizontal position offset of the in-headset notification\n"
	        "overlay as a percentage of the screen half-width ('0' by\n"
	        "default, meaning centred above the DOS picture). Positive\n"
	        "moves right, negative moves left. Valid range from -100\n"
	        "to 100.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_x->SetMinMax(-100, 100);

	auto pint_xr_y = section.AddInt("osd_xr_offset_y", Always, 0);
	pint_xr_y->SetHelp(
	        "Vertical position offset of the in-headset notification\n"
	        "overlay as a percentage of the screen half-height ('0' by\n"
	        "default, meaning above the DOS picture). Positive moves up,\n"
	        "negative moves down. Valid range from -100 to 100.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_y->SetMinMax(-100, 100);
}

// Dedicated swapchain for the overlay-keyboard quad layer (static
// content, filled once) and its enumerated images.
XrSwapchain             sdl_xr_kbd_swapchain = XR_NULL_HANDLE;
std::vector<XrSwapchainImageOpenGLKHR> sdl_xr_kbd_images;
// Notification overlay: dedicated swapchain, texture size, per-message
// lifetime, and the last few messages (newest last).
XrSwapchain             sdl_xr_osd_swapchain = XR_NULL_HANDLE;
std::vector<XrSwapchainImageOpenGLKHR> sdl_xr_osd_images;
std::deque<XrOsdLine> sdl_xr_osd;
// Overlay content lifecycle: setup only creates/enumerates the
// keyboard/OSD swapchains; pixels go in lazily from the first frame
// that needs them (strict runtimes reject acquire/release outside a
// running wait/begin frame).
bool sdl_xr_kbd_filled = false;
bool sdl_xr_osd_dirty  = true;
// Hover key last seen (-1 = none, -2 = nothing seen yet); the texture
// epoch bumps on change so designated images refresh.
int sdl_xr_kbd_hover_built = -2;
// Trigger-held key last seen (same encoding); its letter lights up.
int sdl_xr_kbd_pressed_built = -2;
// Keyboard opacity the texture was built with (-1 = nothing built yet).
float sdl_xr_kbd_opacity_built = -1.0f;
// Current keyboard swapchain dims (follow xr_keyboard_key_scale;
// swapchain is rebuilt live when they no longer match).
int sdl_xr_kbd_tex_w = 0;
int sdl_xr_kbd_tex_h = 0;
// Fresh-by-construction overlays: the image handed to the compositor
// is copied at designate time whenever its epoch lags the texture
// epoch (bulk fills can't rely on acquire order within a frame, so a
// same-image acquire loop can leave stale outlines behind).
GLuint sdl_xr_kbd_tex = 0;
uint64_t sdl_xr_kbd_tex_epoch = 1;
std::vector<uint64_t> sdl_xr_kbd_img_epoch;
GLuint sdl_xr_osd_tex = 0;
uint64_t sdl_xr_osd_tex_epoch = 1;
std::vector<uint64_t> sdl_xr_osd_img_epoch;
// Headset opacity the OSD texture was built with (-1 = nothing built
// yet); the desktop composite rebuilds with full alpha on its own
// signature and flags the texture for a headset refresh (below).
float sdl_xr_osd_opacity_built = -1.0f;
std::string sdl_xr_osd_desk_sig;
bool sdl_xr_osd_desk_touched = false;

// VR menu overlay: dedicated swapchain and fresh-by-construction
// epochs, mirroring the keyboard/OSD layers above.
XrSwapchain             sdl_xr_menu_swapchain = XR_NULL_HANDLE;
std::vector<XrSwapchainImageOpenGLKHR> sdl_xr_menu_images;
bool sdl_xr_menu_filled = false;
GLuint sdl_xr_menu_tex = 0;
uint64_t sdl_xr_menu_tex_epoch = 1;
std::vector<uint64_t> sdl_xr_menu_img_epoch;
std::string sdl_xr_menu_built_sig;
// Pointer-ray ribbons: one tiny shared-gradient swapchain; each hand
// gets its own topmost quad posed along its aim ray every frame.
XrSwapchain             sdl_xr_ray_swapchain = XR_NULL_HANDLE;
std::vector<XrSwapchainImageOpenGLKHR> sdl_xr_ray_images;
bool sdl_xr_ray_filled = false;
GLuint sdl_xr_ray_tex = 0;
uint64_t sdl_xr_ray_tex_epoch = 1;
std::vector<uint64_t> sdl_xr_ray_img_epoch;
// Menu panel geometry in screen-plane units: square panel centred this
// far above the plane centre (the keyboard band hangs below it).

bool sdl_xr_keyboard_visible = false;
int  sdl_xr_keyboard_pressed = -1; // key index held via trigger

// In-headset VR menu state (menu button toggles visibility; the right
// hand aims, the trigger activates). menu_claim_click tells the mouse
// drive to skip a trigger edge the menu consumed; menu_field_focus
// routes overlay-keyboard entry into the preset-name field instead of
// DOS while set.
bool sdl_xr_menu_visible     = false;
bool sdl_xr_menu_claim_click = false;
bool sdl_xr_menu_field_focus = false;
static int  sdl_xr_menu_hover       = -1; // hovered menu row
// Stick navigation: latched cursor row (-1 = follow the ray), last
// stick activity, and per-axis debounce state (latched direction,
// last step time, repeat phase).
static int     sdl_xr_menu_stick_row  = -1;
static int64_t sdl_xr_menu_stick_at   = 0;
static int     sdl_xr_menu_lat_x      = 0;
static int     sdl_xr_menu_lat_y      = 0;
static int64_t sdl_xr_menu_stepx_at  = 0;
static int64_t sdl_xr_menu_stepy_at  = 0;
static bool    sdl_xr_menu_repx       = false;
static bool    sdl_xr_menu_repy       = false;

// Overlay keyboard layout: label, emulated key, row, width units.
// 12 units per row; the overlay covers the bottom 45% of the screen
// plane (v measured upward from the bottom).
// XrKeyDef/Rect live in gui/osd.h now (shared with input mouse drive).

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

// (XrKeyRect moved too; see above.)

// Overlay keyboard geometry: 5 rows of 0.09 in plane-UV filling the
// bottom band (v 0..BandTop) of the screen plane. The keyboard texture
// spans exactly this band, so texture labels map v/BandTop across its
// full height (no wasted empty panel).

const std::vector<XrKeyRect>& openxr_keyboard_rects()
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

int openxr_keyboard_hover()
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

void openxr_drive_keyboard()
{
	if (!sdl_xr_keyboard_visible) {
		return;
	}
	const auto& hand = sdl_xr_hand[1];
	if (!hand.click_ui_changed) {
		return;
	}
	// The menu consumed this edge (ray was on a menu row): neither
	// DOS keys nor field entry may see it.
	if (sdl_xr_menu_claim_click) {
		return;
	}
	// Menu filename entry: keys go to the preset-name field, not DOS.
	if (sdl_xr_menu_visible && sdl_xr_menu_field_focus) {
		if (hand.click_ui) {
			openxr_menu_type_key();
		} else if (sdl_xr_keyboard_pressed >= 0) {
			sdl_xr_keyboard_pressed = -1;
		}
		return;
	}
	if (hand.click_ui) {
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

// (Re)builds the keyboard texture for the given hover outline and
// pressed letter, returning it (0 on failure). Images are (re)filled
// from it at designate time; see the freshness epochs in submit.
GLuint openxr_refresh_keyboard_texture(const int hover_index,
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
	                                                       pressed_index,
	                                                       openxr_keyboard_opacity());
	return kb_tex;
}

bool openxr_setup_osd_layer()
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

bool openxr_setup_keyboard_layer()
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
GLuint openxr_refresh_osd_texture(const float opacity)
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
	                                                      OpenXrOsdTexH,
	                                                      opacity);
	return osd_tex;
}

// Drop expired notifications; shared by the headset submit path and
// the desktop-mirror composite below.
void openxr_osd_prune()
{
	const auto now_us = GetTicksUs();
	while (!sdl_xr_osd.empty() &&
	       now_us - sdl_xr_osd.front().time_us > OpenXrOsdTimeoutUs) {
		sdl_xr_osd.pop_front();
	}
}

// Log + show a notification for XR toggles and setting changes, in
// the headset overlay and/or the desktop-window toasts. Quietly drops
// the overlay part when both are disabled or too early.
void OPENXR_Notify(const std::string& message)
{
	LOG_MSG("OpenXR: %s", message.c_str());
	if (!get_osd_section()->GetBool("osd_xr") &&
	    !get_osd_section()->GetBool("osd_desk")) {
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

// ---------------------------------------------------------------------------
// In-headset VR menu (menu button + right-hand pointer)
// ---------------------------------------------------------------------------
// Opens on the VR menu button (left menu button on Touch/WMR) and floats
// above the overlay keyboard. The right-hand ray (the same pointer
// interface as the VR keyboard) hovers rows; trigger clicks activate
// them. Sliders and lists use their left/right halves for -/+; the XRCFG
// rows carry a SAVE/LOAD button on the right. The filename field takes
// VR-keyboard entry while focused (keys go to the field, not DOS).
static std::string sdl_xr_menu_filename;
static std::vector<std::string> sdl_xr_menu_presets;
static int sdl_xr_menu_load_idx = 0;
static int64_t sdl_xr_menu_presets_at = 0; // last preset re-list time

// Row kinds, mirroring OpenGlRenderer::XrMenuRow (0 header, 1 toggle,
// 2 slider, 3 list, 4 text, 5 button, 6 save, 7 load).
struct XrMenuItem {
	int kind;
	std::string label, value, action;
	bool highlight = false;
	bool active    = false;
};
static std::vector<XrMenuItem> sdl_xr_menu_rows;

static const char* openxr_menu_onoff(const bool on)
{
	return on ? "ON" : "OFF";
}

// Stereo/autodepth cycles with direction (the Ctrl+E / Ctrl+A handlers
// only step forward).
void openxr_menu_cycle_mode(const int dir)
{
	static const char* const modes[] = {"mono",
	                                    "anaglyph",
	                                    "alternating",
	                                    "interleaved",
	                                    "top-bottom",
	                                    "sbs",
	                                    "depth",
	                                    "autodepth",
	                                    "cybermaxx"};
	static constexpr int num_modes = sizeof(modes) / sizeof(modes[0]);
	const std::string curr =
	        get_xr_section()->GetString("xr_stereo_input");
	int at = 0;
	for (int i = 0; i < num_modes; ++i) {
		if (curr == modes[i]) {
			at = i;
			break;
		}
	}
	const char* next = modes[(at + dir % num_modes + num_modes) %
	                         num_modes];
	set_section_property_value("xr", "xr_stereo_input", next);
	OPENXR_Notify(format_str("Stereo input mode: %s", next));
}

void openxr_menu_cycle_autodepth(const int dir)
{
	static const char* const methods[] = {"blend",
	                                      "gradient",
	                                      "luma"
#if C_NEURAL
	                                      ,
	                                      "neural",
	                                      "neural+gradient",
	                                      "neural+luma",
	                                      "neural+blend"
#endif
	                                      };
	static constexpr int num_methods = sizeof(methods) / sizeof(methods[0]);
	const std::string curr =
	        get_xr_section()->GetString("xr_autodepth_method");
	int at = 0;
	for (int i = 0; i < num_methods; ++i) {
		if (curr == methods[i]) {
			at = i;
			break;
		}
	}
	const char* next = methods[(at + dir % num_methods + num_methods) %
	                           num_methods];
	set_section_property_value("xr", "xr_autodepth_method", next);
	OPENXR_Notify(format_str("Autodepth method: %s", next));
}

void openxr_menu_adjust_distance(const char* sec_name,
                                   const char* opt_name,
                                   const int delta)
{
	auto* section = get_section(sec_name);
	assert(section);
	int d = section->GetInt(opt_name) + delta;
	if (d > 500) {
		d = 500;
	}
	if (d < 50) {
		d = 50;
	}
	set_section_property_value(sec_name, opt_name, format_str("%d", d));
}

void openxr_menu_adjust_picture(const int delta)
{
	int size_pct = get_xr_section()->GetInt("xr_picture_size") + delta;
	if (size_pct > 100) {
		size_pct = 100;
	}
	if (size_pct < 20) {
		size_pct = 20;
	}
	set_section_property_value("xr",
	                           "xr_picture_size",
	                           format_str("%d", size_pct));
	OPENXR_Notify(format_str("Picture size %d%%", size_pct));
}

#if C_NEURAL
void openxr_menu_adjust_neural_interval(const int delta)
{
	int ms = get_xr_section()->GetInt("xr_neural_interval_ms") + delta;
	if (ms > 5000) {
		ms = 5000;
	}
	if (ms < 16) {
		ms = 16;
	}
	set_section_property_value("xr",
	                           "xr_neural_interval_ms",
	                           format_str("%d", ms));
}
#endif // C_NEURAL

// Picture lock with Ctrl+0 semantics: enabling re-anchors the screen
// where you are looking right now; disabling just unlocks.
void openxr_menu_toggle_lock()
{
	if (!sdl_xr_world_locked) {
		xr_capture_lock_pose();
		if (sdl_xr_has_lock) {
			sdl_xr_world_locked = true;
			OPENXR_Notify("Screen recentered and locked");
		}
	} else {
		sdl_xr_world_locked = false;
		OPENXR_Notify("Screen position lock off");
	}
}

void openxr_menu_set_visible(const bool show)
{
	if (show == sdl_xr_menu_visible) {
		return;
	}
	sdl_xr_menu_visible = show;
	if (show) {
		sdl_xr_menu_presets    = OPENXR_ListConfigs();
		sdl_xr_menu_load_idx   = 0;
		sdl_xr_menu_presets_at = GetTicksUs();
		sdl_xr_menu_stick_row = -1;
		sdl_xr_menu_stick_at  = 0;
		sdl_xr_menu_lat_x = sdl_xr_menu_lat_y = 0;
		sdl_xr_menu_repx = sdl_xr_menu_repy = false;
		// Centre the DOS sticks and release their buttons while the
		// menu owns the sticks and A/B below.
		JOYSTICK_Move_X(0, 0);
		JOYSTICK_Move_Y(0, 0);
		JOYSTICK_Button(0, 0, false);
		JOYSTICK_Button(0, 1, false);
		JOYSTICK_Move_X(1, 0);
		JOYSTICK_Move_Y(1, 0);
		JOYSTICK_Button(1, 0, false);
		JOYSTICK_Button(1, 1, false);
		// Pointer readiness diagnostic: which binding profile is
		// live and whether the right aim pose is tracked (a dead
		// profile or untracked aim means pointing can't work).
		// Query both hands: a split profile would explain inputs
		// arriving on one hand only.
		std::string profile, left_profile;
		if (sdl_xr_session != XR_NULL_HANDLE) {
			for (int hand = 0; hand < 2; ++hand) {
				XrInteractionProfileState state = {};
				state.type = XR_TYPE_INTERACTION_PROFILE_STATE;
				if (xrGetCurrentInteractionProfile(
				            sdl_xr_session,
				            sdl_xr_hand_path[hand],
				            &state) != XR_SUCCESS ||
				    state.interactionProfile == XR_NULL_PATH) {
					continue;
				}
				char buf[256] = {};
				uint32_t count = 0;
				if (xrPathToString(sdl_xr_instance,
				                   state.interactionProfile,
				                   sizeof(buf),
				                   &count,
				                   buf) != XR_SUCCESS) {
					continue;
				}
				if (hand == 0) {
					left_profile = buf;
				} else {
					profile = buf;
				}
			}
		}
		LOG_MSG("OpenXR: Menu opened; profiles L='%s' R='%s'",
		        left_profile.empty() ? "(none)" : left_profile.c_str(),
		        profile.empty() ? "(none)" : profile.c_str());
		LOG_MSG("OpenXR: Menu opened; right aim %s "
		        "(space %s, locate %d flags 0x%x)",
		        sdl_xr_hand[1].pose_valid ? "tracked" : "NOT TRACKED",
		        sdl_xr_aim_space[1] == XR_NULL_HANDLE ? "MISSING"
		                                             : "ok",
		        (int)sdl_xr_hand[1].aim_result,
		        (unsigned)sdl_xr_hand[1].aim_flags);
		// Boolean liveness per hand: which inputs (if any) have a
		// live source? (Splits "only menu is live" from "pose-only
		// is broken".)
		LOG_MSG("OpenXR: Left live: click=%d grip=%d stick=%d "
		        "joyX=%d joyY=%d menu=%d",
		        (int)openxr_probe_active(sdl_xr_click_action, 0),
		        (int)openxr_probe_active(sdl_xr_grip_action, 0),
		        (int)openxr_probe_active(sdl_xr_stick_action, 0),
		        (int)openxr_probe_active(sdl_xr_joyx_action, 0),
		        (int)openxr_probe_active(sdl_xr_joyy_action, 0),
		        (int)openxr_probe_active(sdl_xr_menu_action, 0));
		LOG_MSG("OpenXR: Right live: click=%d grip=%d joyA=%d joyB=%d",
		        (int)openxr_probe_active(sdl_xr_click_action, 1),
		        (int)openxr_probe_active(sdl_xr_grip_action, 1),
		        (int)openxr_probe_active(sdl_xr_joya_action, 1),
		        (int)openxr_probe_active(sdl_xr_joyb_action, 1));
		// Pose-action liveness per hand: is the aim action bound to a
		// live source right now? (Splits "no binding" from "runtime
		// reports no pose", and a dead hand from a systemic failure.)
		if (sdl_xr_aim_action != XR_NULL_HANDLE) {
			for (int hand = 0; hand < 2; ++hand) {
				XrActionStateGetInfo pose_get = {};
				pose_get.type = XR_TYPE_ACTION_STATE_GET_INFO;
				pose_get.action = sdl_xr_aim_action;
				pose_get.subactionPath = sdl_xr_hand_path[hand];
				XrActionStatePose pose_state = {};
				pose_state.type = XR_TYPE_ACTION_STATE_POSE;
				if (xrGetActionStatePose(sdl_xr_session,
				                         &pose_get,
				                         &pose_state) == XR_SUCCESS) {
					LOG_MSG("OpenXR: %s aim action %s (locate %d "
					        "flags 0x%x)",
					        hand == 0 ? "Left" : "Right",
					        pose_state.isActive ? "ACTIVE"
					                            : "INACTIVE (unbound?)",
					        (int)sdl_xr_hand[hand].aim_result,
					        (unsigned)sdl_xr_hand[hand].aim_flags);
				}
			}
		}
		OPENXR_Notify("VR menu open (menu button closes)");
	} else {
		sdl_xr_menu_field_focus = false;
	}
}

// (Re)builds the menu rows from live settings. Order is fixed so the
// hit-test row index stays meaningful within a frame.
static void openxr_menu_build_rows()
{
	sdl_xr_menu_rows.clear();
	auto* section = get_xr_section();
	const std::string mode = section->GetString("xr_stereo_input");

	sdl_xr_menu_rows.push_back({0, "SCREEN", "", "", false, false});
	sdl_xr_menu_rows.push_back(
	        {2,
	         "Screen distance",
	         format_str("%d cm", section->GetInt("xr_screen_distance")),
	         "",
	         sdl_xr_menu_hover == 1,
	         false});
	sdl_xr_menu_rows.push_back(
	        {2,
	         "Picture size",
	         format_str("%d %%", section->GetInt("xr_picture_size")),
	         "",
	         sdl_xr_menu_hover == 2,
	         false});
	sdl_xr_menu_rows.push_back({1,
	                            "Picture lock",
	                            openxr_menu_onoff(sdl_xr_world_locked),
	                            "",
	                            sdl_xr_menu_hover == 3,
	                            sdl_xr_world_locked});
	sdl_xr_menu_rows.push_back(
	        {2,
	         "KB distance",
	         format_str("%d cm", section->GetInt("xr_kb_distance")),
	         "",
	         sdl_xr_menu_hover == 4,
	         false});
	sdl_xr_menu_rows.push_back(
	        {2,
	         "Toast distance",
	         format_str("%d cm",
	                    get_osd_section()->GetInt("osd_xr_distance")),
	         "",
	         sdl_xr_menu_hover == 5,
	         false});

	sdl_xr_menu_rows.push_back({0, "STEREO", "", "", false, false});
	const bool is_3d = (mode != "mono");
	sdl_xr_menu_rows.push_back({1,
	                            "3D display",
	                            openxr_menu_onoff(is_3d),
	                            "",
	                            sdl_xr_menu_hover == 7,
	                            is_3d});
	sdl_xr_menu_rows.push_back({1,
	                            "Mirror SBS",
	                            openxr_menu_onoff(sdl_xr_mirror_stereo),
	                            "",
	                            sdl_xr_menu_hover == 8,
	                            sdl_xr_mirror_stereo});
	const bool show_depth = section->GetBool("xr_show_depth");
	sdl_xr_menu_rows.push_back({1,
	                            "Depth view",
	                            openxr_menu_onoff(show_depth),
	                            "",
	                            sdl_xr_menu_hover == 9,
	                            show_depth});
	sdl_xr_menu_rows.push_back({3,
	                            "Mode",
	                            mode,
	                            "",
	                            sdl_xr_menu_hover == 10,
	                            false});
	sdl_xr_menu_rows.push_back(
	        {3,
	         "Autodepth",
	         section->GetString("xr_autodepth_method"),
	         "",
	         sdl_xr_menu_hover == 11,
	         false});
#if C_NEURAL
	const std::string neural = openxr_menu_neural_model();
	sdl_xr_menu_rows.push_back(
	        {3,
	         "Neural model",
	         neural.empty() ? "(none)" : neural,
	         "",
	         sdl_xr_menu_hover == 12,
	         false});
	sdl_xr_menu_rows.push_back(
	        {2,
	         "Neural gap",
	         format_str("%d ms",
	                    section->GetInt("xr_neural_interval_ms")),
	         "",
	         sdl_xr_menu_hover == 13,
	         false});
#endif // C_NEURAL
	const bool flipped = section->GetBool("xr_flip_vertical");
	sdl_xr_menu_rows.push_back({1,
	                            "Vert flip",
	                            openxr_menu_onoff(flipped),
	                            "",
	                            sdl_xr_menu_hover ==
	                                    static_cast<int>(
	                                            sdl_xr_menu_rows.size()),
	                            flipped});
	sdl_xr_menu_rows.push_back(
	        {2,
	         "Depth strength",
	         format_str("%d %%", section->GetInt("xr_depth_strength")),
	         "",
	         sdl_xr_menu_hover ==
	                 static_cast<int>(sdl_xr_menu_rows.size()),
	         false});

	sdl_xr_menu_rows.push_back({0, "OTHER", "", "", false, false});
	sdl_xr_menu_rows.push_back(
	        {6,
	         "XRCFG",
	         sdl_xr_menu_filename.empty() ? "_" : sdl_xr_menu_filename,
	         "SAVE",
	         sdl_xr_menu_hover ==
	                 static_cast<int>(sdl_xr_menu_rows.size()),
	         sdl_xr_menu_field_focus});
	const std::string preset =
	        (sdl_xr_menu_presets.empty() ||
	         sdl_xr_menu_load_idx < 0 ||
	         sdl_xr_menu_load_idx >=
	                 static_cast<int>(sdl_xr_menu_presets.size()))
	                ? "(none)"
	                : sdl_xr_menu_presets[sdl_xr_menu_load_idx];
	sdl_xr_menu_rows.push_back(
	        {7,
	         "Preset",
	         preset,
	         "LOAD",
	         sdl_xr_menu_hover ==
	                 static_cast<int>(sdl_xr_menu_rows.size()),
	         false});
	sdl_xr_menu_rows.push_back(
	        {5,
	         "QUIT",
	         "",
	         "",
	         sdl_xr_menu_hover ==
	                 static_cast<int>(sdl_xr_menu_rows.size()),
	         false});
}

// Signature of the current rows (values + hover + focus): the submit
// path rebuilds the texture only when this changes.
std::string openxr_menu_signature()
{
	std::string sig;
	sig.reserve(sdl_xr_menu_rows.size() * 24);
	for (const auto& r : sdl_xr_menu_rows) {
		sig.push_back('0' + r.kind);
		sig += r.label;
		sig.push_back(0);
		sig += r.value;
		sig.push_back(0);
		sig += r.action;
		sig.push_back(r.highlight ? 'H' : 'h');
		sig.push_back(r.active ? 'A' : 'a');
	}
	return sig;
}

// Menu font size multiplier (1.0 = 100%).
static float openxr_menu_font_mult()
{
	return std::clamp(get_xr_section()->GetInt("xr_menu_font_size") /
	                          100.0f,
	                  0.5f,
	                  2.0f);
}

// Menu panel layout from the screen plane, shared by the hit-test
// and the submitted quad so pointing always lands where the panel
// is: full panel size and centre height in world units, plus the
// panel-centre v and v half-span in screen-plane UV units.
void openxr_menu_layout(const float half_h, float& size_out,
                               float& up_out, float& vc_out,
                               float& vhalf_out)
{
	const float menu_s = std::clamp(get_xr_section()->GetInt(
	                                        "xr_menu_scale") /
	                                        100.0f,
	                                0.25f,
	                                2.0f);
	const float menu_oy = std::clamp(get_xr_section()->GetInt(
	                                         "xr_menu_offset_y") /
	                                         100.0f,
	                                 -1.0f,
	                                 1.0f);
	size_out  = OpenXrMenuSizeHalfH * half_h * menu_s;
	up_out    = (OpenXrMenuUpHalfH + menu_oy) * half_h;
	vc_out    = 0.5f + (OpenXrMenuUpHalfH + menu_oy) / 2.0f;
	vhalf_out = size_out / (4.0f * half_h);
}

// Menu-space UV (0..1) of the right-hand ray, or false when the ray
// misses the panel. Geometry matches the submitted quad below.
static bool openxr_menu_uv(float& mu_out, float& mv_out)
{
	float u = 0.0f, v = 0.0f;
	XrPosef anchor = {};
	float half_w = 0.0f, half_h = 0.0f, dist_m = 0.0f;
	if (!openxr_hand_plane_uv(1, u, v) ||
	    !openxr_screen_plane(anchor, half_w, half_h, dist_m)) {
		return false;
	}
	if (half_w <= 0.0f || half_h <= 0.0f) {
		return false;
	}
	float size = 0.0f, up = 0.0f, vc = 0.0f, vhalf = 0.0f;
	openxr_menu_layout(half_h, size, up, vc, vhalf);
	const float mu = (u - 0.5f) * (2.0f * half_w) / size + 0.5f;
	const float mv = (v - vc) / (2.0f * vhalf) + 0.5f;
	if (mu < 0.0f || mu > 1.0f || mv < 0.0f || mv > 1.0f) {
		return false;
	}
	mu_out = mu;
	mv_out = mv;
	return true;
}

// Activate a row: left/right halves step sliders and lists down/up;
// the SAVE/LOAD buttons live on the right of their rows.
static void openxr_menu_activate(const int row, const float mu)
{
	if (row < 0 || row >= static_cast<int>(sdl_xr_menu_rows.size())) {
		return;
	}
	const int kind = sdl_xr_menu_rows[row].kind;
	const bool dec = (mu < 0.5f);
	const int dir  = dec ? -1 : +1;

	switch (kind) {
	case 0: // section header: nothing to do
		return;
	case 1: // toggle
		if (sdl_xr_menu_rows[row].label == "Picture lock") {
			openxr_menu_toggle_lock();
		} else if (sdl_xr_menu_rows[row].label == "3D display") {
			xr_stereo_toggle_handler(true);
		} else if (sdl_xr_menu_rows[row].label == "Mirror SBS") {
			xr_mirror_stereo_handler(true);
		} else if (sdl_xr_menu_rows[row].label == "Depth view") {
			xr_depth_view_handler(true);
		} else if (sdl_xr_menu_rows[row].label == "Vert flip") {
			xr_flip_vertical_handler(true);
		}
		return;
	case 2: // slider
		if (sdl_xr_menu_rows[row].label == "Screen distance") {
			openxr_menu_adjust_distance("xr",
			                            "xr_screen_distance",
			                            25 * dir);
		} else if (sdl_xr_menu_rows[row].label == "Picture size") {
			openxr_menu_adjust_picture(5 * dir);
		} else if (sdl_xr_menu_rows[row].label == "KB distance") {
			openxr_menu_adjust_distance("xr", "xr_kb_distance", 25 * dir);
		} else if (sdl_xr_menu_rows[row].label == "Toast distance") {
			openxr_menu_adjust_distance("osd",
			                            "osd_xr_distance",
			                            25 * dir);
		} else if (sdl_xr_menu_rows[row].label == "Depth strength") {
			xr_depth_strength_adjust(dir > 0, true);
#if C_NEURAL
		} else if (sdl_xr_menu_rows[row].label == "Neural gap") {
			openxr_menu_adjust_neural_interval(100 * dir);
#endif
		}
		return;
	case 3: // list
		if (sdl_xr_menu_rows[row].label == "Mode") {
			openxr_menu_cycle_mode(dir);
		} else if (sdl_xr_menu_rows[row].label == "Autodepth") {
			openxr_menu_cycle_autodepth(dir);
#if C_NEURAL
		} else if (sdl_xr_menu_rows[row].label == "Neural model") {
			xr_neural_cycle_model(dir);
#endif
		}
		return;
	case 4: // (unused: the save row carries its own field)
		return;
	case 5: // button
		if (sdl_xr_menu_rows[row].label == "QUIT") {
			GFX_RequestExit(true);
		}
		return;
	case 6: { // XRCFG save: button zone saves, field zone focuses
		if (mu >= OpenXrMenuButtonZone) {
			if (sdl_xr_menu_filename.empty()) {
				OPENXR_Notify("Type a preset name first "
				              "(point at the keyboard)");
				return;
			}
			if (OPENXR_SaveConfig(sdl_xr_menu_filename)) {
				OPENXR_Notify(format_str("Saved XR preset '%s'",
				                         sdl_xr_menu_filename
				                                 .c_str()));
				sdl_xr_menu_presets = OPENXR_ListConfigs();
				sdl_xr_menu_field_focus = false;
			} else {
				OPENXR_Notify("Preset save failed");
			}
			return;
		}
		sdl_xr_menu_field_focus = true;
		if (!sdl_xr_keyboard_visible) {
			sdl_xr_keyboard_visible = true;
			// Set outside the chord system: the next chord
			// re-applies absolutely.
			sdl_xr_right_chord = -1;
		}
		OPENXR_Notify("Type the preset name (space = _, Enter done)");
		return;
	}
	case 7: { // XRCFG load: button zone loads, halves pick the preset
		if (mu >= OpenXrMenuButtonZone) {
			if (sdl_xr_menu_presets.empty()) {
				OPENXR_Notify("No presets saved yet");
				return;
			}
			if (sdl_xr_menu_load_idx < 0 ||
			    sdl_xr_menu_load_idx >=
			            static_cast<int>(
			                    sdl_xr_menu_presets.size())) {
				sdl_xr_menu_load_idx = 0;
			}
			const std::string& name =
			        sdl_xr_menu_presets[sdl_xr_menu_load_idx];
			if (OPENXR_LoadConfig(name)) {
				OPENXR_Notify(format_str("Loaded XR preset "
				                         "'%s'",
				                         name.c_str()));
			} else {
				OPENXR_Notify("Preset load failed");
			}
			return;
		}
		if (!sdl_xr_menu_presets.empty()) {
			const int n =
			        static_cast<int>(sdl_xr_menu_presets.size());
			sdl_xr_menu_load_idx =
			        (sdl_xr_menu_load_idx + dir % n + n) % n;
		}
		return;
	}
	default:
		return;
	}
}

// Route an overlay-keyboard press into the preset-name field (called
// from the keyboard drive while the field is focused). Only preset
// name characters land in the buffer; Enter/Escape leave the field.
void openxr_menu_type_key()
{
	const int hovered = openxr_keyboard_hover();
	if (hovered < 0) {
		return;
	}
	const auto& rects = openxr_keyboard_rects();
	if (hovered >= static_cast<int>(rects.size())) {
		return;
	}
	const char* label = rects[hovered].def->label;
	sdl_xr_keyboard_pressed = hovered;
	if (strcmp(label, "Bksp") == 0) {
		if (!sdl_xr_menu_filename.empty()) {
			sdl_xr_menu_filename.pop_back();
		}
		return;
	}
	if (strcmp(label, "Ent") == 0 || strcmp(label, "Esc") == 0) {
		sdl_xr_menu_field_focus = false;
		return;
	}
	char c = 0;
	if (label[0] != 0 && label[1] == 0) {
		c = label[0];
	} else if (strcmp(label, "Spc") == 0) {
		c = '_'; // spaces are stripped from preset file names
	}
	const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	                   (c >= '0' && c <= '9') || c == '_' || c == '-';
	if (valid &&
	    static_cast<int>(sdl_xr_menu_filename.size()) <
	            OpenXrMenuMaxNameLen) {
		sdl_xr_menu_filename.push_back(c);
	}
}

void openxr_drive_menu()
{
	sdl_xr_menu_claim_click = false;
	// Menu button edges (either hand; only bound hands report).
	for (int hand = 0; hand < 2; ++hand) {
		if (sdl_xr_hand[hand].menu_changed && sdl_xr_hand[hand].menu) {
			openxr_menu_set_visible(!sdl_xr_menu_visible);
		}
	}
	if (!sdl_xr_menu_visible) {
		sdl_xr_menu_hover     = -1;
		sdl_xr_menu_stick_row = -1;
		return;
	}
	// Re-list presets every couple of seconds so files saved or
	// removed elsewhere (desktop XRCFG, file manager) show up
	// without reopening the menu. All other rows re-read their live
	// settings every frame in openxr_menu_build_rows below, so the
	// menu always mirrors the current values.
	static constexpr int64_t PresetsRefreshUs = 2'000'000;
	const int64_t menu_now_us = GetTicksUs();
	if (menu_now_us - sdl_xr_menu_presets_at > PresetsRefreshUs) {
		sdl_xr_menu_presets_at = menu_now_us;
		sdl_xr_menu_presets    = OPENXR_ListConfigs();
		if (sdl_xr_menu_load_idx >=
		    static_cast<int>(sdl_xr_menu_presets.size())) {
			sdl_xr_menu_load_idx = 0;
		}
	}
	openxr_menu_build_rows();
	const int nrows = static_cast<int>(sdl_xr_menu_rows.size());
	// Ray hover first (may be overridden by stick navigation below).
	float mu = 0.5f, mv = 0.5f;
	int hover = -1;
	if (nrows > 0 && openxr_menu_uv(mu, mv)) {
		int row = static_cast<int>((1.0f - mv) * nrows);
		if (row < 0) {
			row = 0;
		}
		if (row >= nrows) {
			row = nrows - 1;
		}
		hover = row;
	}
	// Stick navigation with debounce: either stick moves the cursor
	// (up/down step over interactive rows, left/right adjusts), A
	// confirms, B goes back (unfocus, else close). Either stick
	// works; the stronger deflection wins when both move. A direction
	// latches past the press threshold and re-arms only below the
	// release threshold, so jitter around the edge cannot
	// multi-step; holding a direction still repeats slowly after a
	// beat for long lists.
	const int64_t now_us = GetTicksUs();
	float sx = sdl_xr_joy_x, sy = sdl_xr_joy_y;
	if (std::fabs(sdl_xr_rjoy_x) > std::fabs(sx)) {
		sx = sdl_xr_rjoy_x;
	}
	if (std::fabs(sdl_xr_rjoy_y) > std::fabs(sy)) {
		sy = sdl_xr_rjoy_y;
	}
	static constexpr float StickPress   = 0.6f;
	static constexpr float StickRelease = 0.35f;
	static constexpr int64_t StickHoldUs   = 600'000; // first repeat
	static constexpr int64_t StickRepeatUs = 300'000; // held repeat
	static constexpr int64_t StickResumeUs = 800'000; // ray takes over
	auto step_cursor = [&](const int dir) {
		// Latch from the ray (or the first row) so steps continue
		// from what is highlighted now; skip headers, stop at ends.
		if (sdl_xr_menu_stick_row < 0) {
			sdl_xr_menu_stick_row = (hover >= 0) ? hover : 0;
		}
		int row = sdl_xr_menu_stick_row;
		for (int guard = 0; guard < nrows; ++guard) {
			row += dir;
			if (row < 0 || row >= nrows) {
				break;
			}
			if (sdl_xr_menu_rows[row].kind != 0) {
				break;
			}
		}
		if (row >= 0 && row < nrows &&
		    sdl_xr_menu_rows[row].kind != 0) {
			sdl_xr_menu_stick_row = row;
			hover                 = row;
		}
		sdl_xr_menu_stick_at = now_us;
	};
	auto adjust_at = [&](const int dir) {
		const int at = (sdl_xr_menu_stick_row >= 0)
		                       ? sdl_xr_menu_stick_row
		                       : hover;
		if (at >= 0) {
			// Right side is increase/next/SAVE/LOAD, like the
			// trigger on the right half.
			openxr_menu_activate(at, dir > 0 ? 1.0f : 0.0f);
			hover = at;
		}
		sdl_xr_menu_stick_at = now_us;
	};
	// Y steps the cursor (stick down/toward is +1).
	if (sy > StickPress && sdl_xr_menu_lat_y != +1) {
		sdl_xr_menu_lat_y = +1;
		step_cursor(+1);
		sdl_xr_menu_stepy_at = now_us;
		sdl_xr_menu_repy     = false;
	} else if (sy < -StickPress && sdl_xr_menu_lat_y != -1) {
		sdl_xr_menu_lat_y = -1;
		step_cursor(-1);
		sdl_xr_menu_stepy_at = now_us;
		sdl_xr_menu_repy     = false;
	} else if (sdl_xr_menu_lat_y != 0 && std::fabs(sy) < StickRelease) {
		sdl_xr_menu_lat_y = 0; // re-arm
		sdl_xr_menu_repy  = false;
	} else if (sdl_xr_menu_lat_y != 0 && std::fabs(sy) > StickPress &&
	           now_us - sdl_xr_menu_stepy_at >=
	                   (sdl_xr_menu_repy ? StickRepeatUs
	                                     : StickHoldUs)) {
		step_cursor(sdl_xr_menu_lat_y);
		sdl_xr_menu_stepy_at = now_us;
		sdl_xr_menu_repy     = true;
	}
	// X adjusts the cursor row in place.
	if (sx > StickPress && sdl_xr_menu_lat_x != +1) {
		sdl_xr_menu_lat_x = +1;
		adjust_at(+1);
		sdl_xr_menu_stepx_at = now_us;
		sdl_xr_menu_repx     = false;
	} else if (sx < -StickPress && sdl_xr_menu_lat_x != -1) {
		sdl_xr_menu_lat_x = -1;
		adjust_at(-1);
		sdl_xr_menu_stepx_at = now_us;
		sdl_xr_menu_repx     = false;
	} else if (sdl_xr_menu_lat_x != 0 && std::fabs(sx) < StickRelease) {
		sdl_xr_menu_lat_x = 0; // re-arm
		sdl_xr_menu_repx  = false;
	} else if (sdl_xr_menu_lat_x != 0 && std::fabs(sx) > StickPress &&
	           now_us - sdl_xr_menu_stepx_at >=
	                   (sdl_xr_menu_repx ? StickRepeatUs
	                                     : StickHoldUs)) {
		adjust_at(sdl_xr_menu_lat_x);
		sdl_xr_menu_stepx_at = now_us;
		sdl_xr_menu_repx     = true;
	}
	if (sdl_xr_menu_stick_row >= 0 &&
	    now_us - sdl_xr_menu_stick_at <= StickResumeUs) {
		hover = sdl_xr_menu_stick_row;
	} else {
		// Ray resumes once the sticks rest.
		sdl_xr_menu_stick_row = -1;
	}
	sdl_xr_menu_hover = hover;
	if (hover < 0) {
		return;
	}
	// Rebuild with the hover highlight before any activation below.
	openxr_menu_build_rows();
	// Trigger press on a row activates it and consumes the edge so
	// the DOS mouse does not click along. Uses the firmer UI-grade
	// press (xr_ui_trigger_threshold).
	const auto& right = sdl_xr_hand[1];
	if (right.click_ui_changed && right.click_ui) {
		openxr_menu_activate(hover, mu);
		sdl_xr_menu_claim_click = true;
	}
	// A confirms the row (right side, like the trigger there); B
	// backs out (unfocus the name field, else close the menu). DOS
	// joystick buttons and grip chords stay frozen while the menu is
	// open (see the drive gates below), so these never leak to DOS.
	if (sdl_xr_joy_btn_pressed[0]) {
		openxr_menu_activate(hover, 1.0f);
	} else if (sdl_xr_joy_btn_pressed[1]) {
		if (sdl_xr_menu_field_focus) {
			sdl_xr_menu_field_focus = false;
		} else {
			openxr_menu_set_visible(false);
		}
	}
}

// (Re)builds the menu texture from the current rows. Images are
// (re)filled from it at designate time; see the freshness epochs in
// the submit path.
GLuint openxr_refresh_menu_texture()
{
	if (sdl_xr_menu_swapchain == XR_NULL_HANDLE ||
	    sdl.renderer == nullptr ||
	    sdl.render_backend_type != RenderBackendType::OpenGl) {
		return 0;
	}
	auto* gl_renderer = static_cast<OpenGlRenderer*>(sdl.renderer.get());

	openxr_menu_build_rows();
	std::vector<OpenGlRenderer::XrMenuRow> xr_rows;
	xr_rows.reserve(sdl_xr_menu_rows.size());
	for (const auto& r : sdl_xr_menu_rows) {
		xr_rows.push_back({r.kind,
		                   r.label.c_str(),
		                   r.value.c_str(),
		                   r.action.c_str(),
		                   r.highlight,
		                   r.active});
	}
	return gl_renderer->UpdateXrMenuTexture(xr_rows.data(),
	                                        static_cast<int>(xr_rows.size()),
	                                        OpenXrMenuTexW,
	                                        OpenXrMenuTexH,
	                                        openxr_menu_font_mult());
}

bool openxr_setup_menu_layer()
{
	if (sdl_xr_session == XR_NULL_HANDLE) {
		return false;
	}
	if (sdl_xr_menu_swapchain != XR_NULL_HANDLE) {
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
	info.width       = OpenXrMenuTexW;
	info.height      = OpenXrMenuTexH;
	info.faceCount   = 1;
	info.arraySize   = 1;
	info.mipCount    = 1;

	if (xrCreateSwapchain(sdl_xr_session, &info, &sdl_xr_menu_swapchain) !=
	    XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to create menu swapchain");
		sdl_xr_menu_swapchain = XR_NULL_HANDLE;
		return false;
	}

	uint32_t image_count = 0;
	if (xrEnumerateSwapchainImages(sdl_xr_menu_swapchain,
	                              0,
	                              &image_count,
	                              nullptr) != XR_SUCCESS ||
	    image_count == 0) {
		LOG_ERR("OpenXR: Failed to enumerate menu swapchain images");
		return false;
	}
	sdl_xr_menu_images.resize(image_count);
	for (auto& img : sdl_xr_menu_images) {
		img.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
		img.next = nullptr;
	}
	if (xrEnumerateSwapchainImages(
	            sdl_xr_menu_swapchain,
	            image_count,
	            &image_count,
	            reinterpret_cast<XrSwapchainImageBaseHeader*>(
	                    sdl_xr_menu_images.data())) != XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to enumerate menu swapchain images");
		sdl_xr_menu_images.clear();
		return false;
	}

	// Content is filled lazily from the first frame that shows the
	// menu; acquiring a swapchain image outside a running frame may
	// be rejected by strict runtimes.
	sdl_xr_menu_img_epoch.assign(image_count, 0);
	LOG_MSG("OpenXR: Menu layer ready (%dx%d, %u images)",
	        OpenXrMenuTexW,
	        OpenXrMenuTexH,
	        image_count);
	return true;
}

// ---------------------------------------------------------------------------
// 3D pointer-ray ribbons (one quad per hand, submitted after every
// other quad so the rays stay visible over the keyboard/OSD/menu).
// ---------------------------------------------------------------------------
// Ribbon pose for one hand: quad X along the aim ray from the
// controller to the screen-plane hit (or a fixed length when the ray
// misses), Y across (head-facing), origin at the segment midpoint.
// False when the hand has no usable aim.
bool openxr_ray_ribbon(const int hand, XrPosef& pose_out,
                              float& len_out)
{
	if (hand < 0 || hand > 1 || !sdl_xr_hand[hand].pose_valid) {
		return false;
	}
	const auto& hp = sdl_xr_hand[hand].pose;
	const XrVec3 o   = {hp.position.x, hp.position.y, hp.position.z};
	const XrVec3 dir = xr_quat_rotate(hp.orientation,
	                                   {0.0f, 0.0f, -1.0f});

	float len = OpenXrRayMaxLenM;
	XrVec3 from = {}, rd = {}, hit = {};
	float u = 0.0f, v = 0.0f;
	if (openxr_hand_ray(hand, from, rd, hit, u, v)) {
		len = xr_len(xr_sub(hit, o));
		if (len > OpenXrRayMaxLenM) {
			len = OpenXrRayMaxLenM;
		}
	}
	if (len < 0.05f) {
		return false;
	}
	const XrVec3 tip = {o.x + dir.x * len,
	                    o.y + dir.y * len,
	                    o.z + dir.z * len};
	const XrVec3 mid = {(o.x + tip.x) * 0.5f,
	                    (o.y + tip.y) * 0.5f,
	                    (o.z + tip.z) * 0.5f};

	// Face the head: width axis perpendicular to the ray in the
	// head-facing plane (eye midpoint, always located in-frame).
	// Right-handed basis (X=ray, Y=width, Z=X cross Y) with Z facing
	// the head, so the quad front-face is never culled.
	const XrVec3 eye_mid = {
	        (sdl_xr_views[0].pose.position.x +
	         sdl_xr_views[1].pose.position.x) *
	                0.5f,
	        (sdl_xr_views[0].pose.position.y +
	         sdl_xr_views[1].pose.position.y) *
	                0.5f,
	        (sdl_xr_views[0].pose.position.z +
	         sdl_xr_views[1].pose.position.z) *
	                0.5f};
	const XrVec3 x  = xr_norm(xr_sub(tip, o));
	const XrVec3 to = xr_norm(xr_sub(eye_mid, mid));
	XrVec3 w        = xr_cross(to, x);
	if (xr_len(w) < 1e-6f) {
		// Ray aimed at the head: fall back to world up.
		w = xr_cross({0.0f, 1.0f, 0.0f}, x);
		if (xr_len(w) < 1e-6f) {
			w = xr_cross({1.0f, 0.0f, 0.0f}, x);
		}
	}
	const XrVec3 wu = xr_norm(w);
	const XrVec3 n  = xr_cross(x, wu);
	// Rotation columns X=x, Y=wu, Z=n (column-major).
	const float rot[9] = {x.x,  x.y,  x.z,
	                      wu.x, wu.y, wu.z,
	                      n.x,  n.y,  n.z};
	pose_out.orientation = xr_mat3_to_quat(rot);
	pose_out.position    = {mid.x, mid.y, mid.z};
	len_out              = len;
	return true;
}

GLuint openxr_refresh_ray_texture()
{
	if (sdl_xr_ray_swapchain == XR_NULL_HANDLE ||
	    sdl.renderer == nullptr ||
	    sdl.render_backend_type != RenderBackendType::OpenGl) {
		return 0;
	}
	auto* gl_renderer = static_cast<OpenGlRenderer*>(sdl.renderer.get());
	return gl_renderer->GetXrRayTexture();
}

bool openxr_setup_ray_layer()
{
	if (sdl_xr_session == XR_NULL_HANDLE) {
		return false;
	}
	if (sdl_xr_ray_swapchain != XR_NULL_HANDLE) {
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
	info.width       = OpenXrRayTexW;
	info.height      = OpenXrRayTexH;
	info.faceCount   = 1;
	info.arraySize   = 1;
	info.mipCount    = 1;

	if (xrCreateSwapchain(sdl_xr_session, &info, &sdl_xr_ray_swapchain) !=
	    XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to create ray swapchain");
		sdl_xr_ray_swapchain = XR_NULL_HANDLE;
		return false;
	}

	uint32_t image_count = 0;
	if (xrEnumerateSwapchainImages(sdl_xr_ray_swapchain,
	                              0,
	                              &image_count,
	                              nullptr) != XR_SUCCESS ||
	    image_count == 0) {
		LOG_ERR("OpenXR: Failed to enumerate ray swapchain images");
		return false;
	}
	sdl_xr_ray_images.resize(image_count);
	for (auto& img : sdl_xr_ray_images) {
		img.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
		img.next = nullptr;
	}
	if (xrEnumerateSwapchainImages(
	            sdl_xr_ray_swapchain,
	            image_count,
	            &image_count,
	            reinterpret_cast<XrSwapchainImageBaseHeader*>(
	                    sdl_xr_ray_images.data())) != XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to enumerate ray swapchain images");
		sdl_xr_ray_images.clear();
		return false;
	}

	sdl_xr_ray_img_epoch.assign(image_count, 0);
	LOG_MSG("OpenXR: Ray layer ready (%dx%d, %u images)",
	        OpenXrRayTexW,
	        OpenXrRayTexH,
	        image_count);
	return true;
}

#if C_OPENXR

// Mirror-window toasts: composite the notification overlay over the
// desktop framebuffer (CPU blend, top-centre) before upload. Rebuilds
// the raster only when the messages change; transparency applies at
// blend time so it never needs a rebuild.
void openxr_composite_desk_osd()
{
	static const bool use_openxr =
	        (get_sdl_section()->GetString("output") == "openxr");
	if (!use_openxr || !get_osd_section()->GetBool("osd_desk")) {
		return;
	}
	openxr_osd_prune();
	if (sdl_xr_osd.empty()) {
		return;
	}
	if (sdl.renderer == nullptr ||
	    sdl.render_backend_type != RenderBackendType::OpenGl) {
		return;
	}
	int desk_op = get_osd_section()->GetInt("osd_desk_opacity");
	if (desk_op < 0) {
		desk_op = 0;
	}
	if (desk_op > 100) {
		desk_op = 100;
	}
	if (desk_op <= 0) {
		return; // fully transparent: nothing to draw
	}
	auto* gl_renderer = static_cast<OpenGlRenderer*>(sdl.renderer.get());
	// Signature covers the messages plus the headset opacity (which
	// shares the texture): any change rebuilds full-alpha here and
	// flags the headset side for its own refresh.
	std::string sig;
	for (const auto& line : sdl_xr_osd) {
		sig += line.text;
		sig.push_back(0);
	}
	sig += std::to_string(
	        static_cast<int>(openxr_osd_opacity() * 100.0f + 0.5f));
	if (sig != sdl_xr_osd_desk_sig) {
		std::vector<const char*> lines;
		lines.reserve(sdl_xr_osd.size());
		for (auto it = sdl_xr_osd.rbegin();
		     it != sdl_xr_osd.rend();
		     ++it) {
			lines.push_back(it->text.c_str());
		}
		if (gl_renderer->UpdateXrOsdTexture(lines.data(),
		                                    lines.size(),
		                                    OpenXrOsdTexW,
		                                    OpenXrOsdTexH,
		                                    1.0f) == 0) {
			return;
		}
		sdl_xr_osd_desk_sig     = sig;
		sdl_xr_osd_desk_touched = true;
	}
	const uint32_t* px = nullptr;
	int w = 0, h = 0;
	gl_renderer->GetXrOsdRaster(px, w, h);
	if (!px) {
		return;
	}
	gl_renderer->CompositeDeskOsd(px,
	                              w,
	                              h,
	                              desk_op / 100.0f,
	                              std::clamp(get_osd_section()->GetInt(
	                                                 "osd_desk_scale") /
	                                                 100.0f,
	                                         0.25f,
	                                         2.0f));
}
#endif // C_OPENXR

// XR preset files ("xrcfg"): the [xr] and [osd] sections saved/loaded
// with the same key names as the main config. Used by the XRCFG command.
static std_fs::path openxr_cfg_path(const std::string& name, bool& ok)
{
	std::string safe;
	for (const char c : name) {
		const bool valid =
		        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		        (c >= '0' && c <= '9') || c == '_' || c == '-';
		if (valid) {
			safe.push_back(c);
		}
	}
	ok = !safe.empty();
	std_fs::path dir = get_config_dir() / "xrcfg";
	return dir / (safe + ".xrcfg");
}

bool OPENXR_SaveConfig(const std::string& name)
{
	bool ok = false;
	const auto path = openxr_cfg_path(name, ok);
	if (!ok) {
		LOG_ERR("OpenXR: Bad xrcfg preset name '%s'", name.c_str());
		return false;
	}
	auto* section = get_section("xr");
	auto* sec     = dynamic_cast<SectionProp*>(section);
	if (!sec) {
		LOG_ERR("OpenXR: No [xr] section to save");
		return false;
	}
	auto* osection = get_section("osd");
	auto* osec     = dynamic_cast<SectionProp*>(osection);
	if (!osec) {
		LOG_ERR("OpenXR: No [osd] section to save");
		return false;
	}
	std::error_code ec;
	std_fs::create_directories(path.parent_path(), ec);
	if (ec) {
		LOG_ERR("OpenXR: Cannot create xrcfg dir: %s",
		        ec.message().c_str());
		return false;
	}
	std::ofstream out(path);
	if (!out) {
		LOG_ERR("OpenXR: Cannot write %s", path.string().c_str());
		return false;
	}
	out << "# DOSBox Staging XR preset (" << path.stem().string()
	    << ")\n[xr]\n";
	for (size_t i = 0; sec->GetProperty(i); ++i) {
		const auto* prop = sec->GetProperty(i);
		if (prop->IsDeprecated()) {
			continue;
		}
		out << prop->propname << " = " << prop->GetValue().ToString()
		    << "\n";
	}
	out << "[osd]\n";
	for (size_t i = 0; osec->GetProperty(i); ++i) {
		const auto* prop = osec->GetProperty(i);
		if (prop->IsDeprecated()) {
			continue;
		}
		out << prop->propname << " = " << prop->GetValue().ToString()
		    << "\n";
	}
	out.close();
	LOG_MSG("OpenXR: Saved XR preset to %s", path.string().c_str());
	return true;
}

bool OPENXR_LoadConfig(const std::string& name)
{
	bool ok = false;
	const auto path = openxr_cfg_path(name, ok);
	if (!ok) {
		LOG_ERR("OpenXR: Bad xrcfg preset name '%s'", name.c_str());
		return false;
	}
	std::ifstream in(path);
	if (!in) {
		LOG_ERR("OpenXR: Cannot read %s", path.string().c_str());
		return false;
	}
	int applied = 0;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		const auto first = line.find_first_not_of(" \t");
		if (first == std::string::npos || line[first] == '#' ||
		    line[first] == '[') {
			continue;
		}
		const auto eq = line.find('=', first);
		if (eq == std::string::npos) {
			continue;
		}
		auto key = line.substr(first, eq - first);
		auto val = line.substr(eq + 1);
		const auto key_end = key.find_last_not_of(" \t");
		key.resize(key_end + 1);
		const auto val_start = val.find_first_not_of(" \t");
		if (val_start == std::string::npos) {
			continue;
		}
		val = val.substr(val_start);
		const auto val_end = val.find_last_not_of(" \t");
		val.resize(val_end + 1);
		// osd_* keys live in the [osd] section, the rest in [xr].
		const std::string sec_name =
		        (key.rfind("osd_", 0) == 0) ? "osd" : "xr";
		set_section_property_value(sec_name, key.c_str(), val.c_str());
		++applied;
	}
	LOG_MSG("OpenXR: Applied %d XR settings from %s",
	        applied,
	        path.string().c_str());
	return applied > 0;
}

std::vector<std::string> OPENXR_ListConfigs()
{
	std::vector<std::string> names;
	std::error_code ec;
	const std_fs::path dir = get_config_dir() / "xrcfg";
	if (!std_fs::exists(dir, ec)) {
		return names;
	}
	for (const auto& entry : std_fs::directory_iterator(dir, ec)) {
		if (!entry.is_regular_file(ec)) {
			continue;
		}
		const auto& p = entry.path();
		if (p.extension() == ".xrcfg") {
			names.push_back(p.stem().string());
		}
	}
	std::sort(names.begin(), names.end());
	return names;
}

// HUD layer teardown (called from openxr_teardown_session).
void osd_teardown_layers()
{
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
	sdl_xr_menu_visible     = false;
	sdl_xr_menu_field_focus = false;
	sdl_xr_menu_hover       = -1;
	if (sdl_xr_ray_swapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(sdl_xr_ray_swapchain);
		sdl_xr_ray_swapchain = XR_NULL_HANDLE;
	}
	sdl_xr_ray_images.clear();
	sdl_xr_ray_filled = false;
	sdl_xr_ray_img_epoch.clear();
}

#endif // C_OPENXR
// Key-cell scale: texture resolution and panel size grow with it so
// keys stay crisp. Returns the multiplier (1.0 = 100%).
float openxr_keyboard_key_scale()
{
	return std::clamp(get_xr_section()->GetInt("xr_keyboard_key_scale") /
	                          100.0f,
	                  0.5f,
	                  2.0f);
}

// Runtime keyboard texture dims (base 1000x500 scaled by key scale).
void openxr_keyboard_tex_size(int& w_out, int& h_out)
{
	const float s = openxr_keyboard_key_scale();
	w_out = static_cast<int>(OpenXrKeyboardTexW * s + 0.5f);
	h_out = static_cast<int>(OpenXrKeyboardTexH * s + 0.5f);
}

// Keyboard opacity for compositor blending (0.1 = 10% .. 1.0 = opaque).
float openxr_keyboard_opacity()
{
	return std::clamp(get_xr_section()->GetInt("xr_keyboard_opacity") /
	                          100.0f,
	                  0.1f,
	                  1.0f);
}

// Notification-overlay opacity, same convention (1.0 = opaque).
float openxr_osd_opacity()
{
	return std::clamp(get_osd_section()->GetInt("osd_xr_opacity") /
	                          100.0f,
	                  0.1f,
	                  1.0f);
}
