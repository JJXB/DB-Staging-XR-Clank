// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// OpenXR session core: instance, session, spaces, swapchains, views,
// projections, frame loop, events, recovery, and the [xr] config
// section. HUD layers (osd), input (xrinput), stereo presentation
// (xrstereo), and neural depth (xrneural) plug in through here.

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

// Session lifecycle (called from SDL glue).
void init_openxr();
void cleanup_openxr();

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
float openxr_layer_dist(const char* opt_name);

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
extern std::vector<XrSwapchainImageOpenGLKHR> sdl_xr_swapchain_images;
extern int64_t      sdl_xr_swapchain_format;
extern uint32_t     sdl_xr_swapchain_width;
extern uint32_t     sdl_xr_swapchain_height;
extern uint64_t     sdl_xr_stat_blit_us;
extern uint64_t     sdl_xr_stat_mirror_us;
extern bool         sdl_xr_submit_blank;

#endif // C_OPENXR

#endif
