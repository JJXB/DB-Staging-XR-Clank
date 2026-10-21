// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// OpenXR session core: instance, session, spaces, swapchains, views,
// projections, frame loop, events, recovery, and the [xr] config
// section. HUD layers (osd), input (xrinput) and stereo presentation
// (xrstereo) plug in through here.

#ifndef DOSBOX_XR_H
#define DOSBOX_XR_H

#include <string>
#include <vector>

#include "config/setup.h"
#include "private/sdl_gui.h"

#if C_OPENXR
#include <openxr/openxr.h>

#if defined(_WIN32)
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
// used by XR_KHR_vulkan_enable are visible (headers from vcpkg
// vulkan-headers; the loader itself is dynamic, see gui/xrvulkan).
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#if C_XR_D3D11 || C_XR_D3D12
// d3d11.h/d3d12.h must precede the OpenXR platform header so the D3D
// types used by XR_KHR_D3D11_enable/XR_KHR_D3D12_enable are visible
// (Windows SDK inbox headers).
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
#include <openxr/openxr_platform.h>

// [xr] section accessor (the section always exists; values are live).
SectionProp* get_xr_section();

// [xr] config registration (called from GFX_AddConfigSection).
void init_xr_config_settings(SectionProp& section);

// Which graphics API backs the XR session (follows the desktop
// render backend: OpenGL output -> OpenGL binding, Vulkan output ->
// Vulkan binding, D3D11/12 outputs -> D3D11/12 bindings, anything
// else -> no headset).
enum class XrGfxBackend { None, OpenGl, Vulkan, D3D11, D3D12 };
XrGfxBackend openxr_gfx_backend();

// Backend-neutral swapchain image (GLuint texture or VkImage, both
// fit in the handle). Enumerated per session; a session never mixes
// backends.
struct XrGfxImage {
	XrStructureType type   = XR_TYPE_UNKNOWN;
	uint64_t        handle = 0;
};

// Enumerate a swapchain's images into neutral handles for the active
// backend (resets Vulkan layout tracking for them).
bool openxr_enumerate_images(XrSwapchain swapchain, const char* label,
                             std::vector<XrGfxImage>& out);

// Session lifecycle (called from SDL glue).
void init_openxr();
void cleanup_openxr();

// Headset master switch (live [xr] xr_enabled value): bring the
// session up or tear it down immediately. Safe to call redundantly.
void openxr_set_enabled(const bool on);

// Live [xr] key-change handler (registered on the section; DOS
// `config -set` runs it, programmatic sets call openxr_set_enabled
// directly).
void notify_xr_setting_updated(SectionProp& section, const std::string& prop_name);

// Per-frame session driver (called from GFX_MaybePresentFrame).
bool openxr_frame_begin(XrTime& display_time_out);
void openxr_frame_end(const XrTime display_time);
void openxr_maybe_recover();

// Head tracking (fed by the frame loop, read by input/mouse code).
void openxr_update_head_tracking();

// Composition-layer placement helpers (used by HUD layers, input
// rays, and frame submission).
bool  openxr_use_world_lock();
bool  openxr_screen_anchor(XrPosef& anchor_out);
bool  openxr_screen_plane(XrPosef& anchor_out, float& half_w_out,
                          float& half_h_out, float& dist_m_out);
float openxr_layer_dist(const char* opt_name,
                        SectionProp* section = nullptr);

// World-space pose/quad size of the overlay keyboard, shared by the
// layer submit and the aim-ray key hit-testing.
bool openxr_keyboard_quad(XrPosef& pose_out, float& width_out,
                          float& height_out);

// Grip-adjustable layout for the screen, overlay keyboard, and VR
// menu. Offsets are fractions of the base screen-plane half extents;
// scale is a multiplier applied about the element's own centre.
struct XrPanelLayout {
	float offset_x = 0.0f;
	float offset_y = 0.0f;
	float scale    = 1.0f;
};

extern XrPanelLayout sdl_xr_layout_screen;
extern XrPanelLayout sdl_xr_layout_keyboard;
extern XrPanelLayout sdl_xr_layout_menu;

// Load the runtime layouts from the [xr] config once per session.
void openxr_layout_load();

// Persist one panel's runtime layout back to the [xr] config.
// panel: 0 = screen, 1 = keyboard, 2 = menu.
void openxr_layout_save(int panel);

// World-space pose and size of the cropped DOS picture panel. False
// when the picture rect is not known yet (falls back to projection).
bool openxr_screen_panel(XrPosef& pose_out, float& width_out,
                         float& height_out);

// In-headset mapper panel. The mapper UI renders with its own SDL
// renderer while the normal frame loop is paused, so this path keeps
// the XR session alive and submits the captured UI as a quad in front
// of the user. All functions are no-ops when the session is not ready.
//
// MapperBegin: mark the mapper active and remember the game/mapper GL
// contexts (only needed by the OpenGL XR backend; pass nullptr when
// the mapper did not create its own renderer).
void openxr_mapper_begin(void* game_gl_context, void* mapper_gl_context);

// Capture the current mapper output (call right after drawing, before
// presenting) into the CPU buffer used by the panel.
void openxr_mapper_capture(SDL_Renderer* renderer);

// Keep XR frames flowing and present the last capture. Call once per
// repetition of the mapper UI loop.
void openxr_mapper_frame(SDL_Renderer* renderer);

// Leave mapper mode (subsequent normal frames resume).
void openxr_mapper_end();

// Shared session state (owned here, read across modules).
extern SDL_Block    sdl;
extern XrInstance   sdl_xr_instance;
extern XrSystemId   sdl_xr_systemId;
extern XrSession    sdl_xr_session;
extern XrSpace      sdl_xr_app_space;
extern bool         sdl_xr_disabled;
extern bool         sdl_xr_session_ready;
extern bool         sdl_xr_initialized;
extern XrView       sdl_xr_views[2];
extern uint32_t     sdl_xr_view_count;
extern bool         sdl_xr_world_locked;
extern bool         sdl_xr_has_lock;
extern XrPosef      sdl_xr_locked_pose[2];
extern XrFovf       sdl_xr_locked_fov[2];
extern XrPosef      sdl_xr_head_pose;
extern bool         sdl_xr_head_pose_valid;
extern XrSwapchain  sdl_xr_swapchain;
extern int          sdl_xr_active_image;
extern std::vector<XrGfxImage> sdl_xr_swapchain_images;
extern int64_t      sdl_xr_swapchain_format;
extern uint32_t     sdl_xr_swapchain_width;
extern uint32_t     sdl_xr_swapchain_height;
extern uint64_t     sdl_xr_stat_blit_us;
extern uint64_t     sdl_xr_stat_mirror_us;
extern bool         sdl_xr_submit_blank;

#endif // C_OPENXR

#endif
