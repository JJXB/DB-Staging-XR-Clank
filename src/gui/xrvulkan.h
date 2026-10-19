// SPDX-FileCopyrightText:  2025-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Raw Vulkan device for the OpenXR Vulkan graphics binding
// (XR_KHR_vulkan_enable) plus a transfer-only compositor: CPU pixels
// are staged through a host-visible buffer and copied/blitted into
// swapchain VkImages. No shaders, no render passes, no WSI surface -
// swapchain images come from the XR runtime, and the desktop window
// keeps rendering through the normal RenderBackend (the Vulkan
// renderer when output=vulkan).
//
// Vulkan is loaded dynamically (vulkan-1.dll / libvulkan.so.1), so
// systems without Vulkan still run: every entry point reports
// failure with a log line and the XR layer stays down.
//
// Pixel contract: all CPU buffers are bottom-up rows (matching the
// shared xr_raster builders and the xrstereo decode buffers).
// Rasters are RGBA byte order, game/eye frames BGRA byte order;
// conversion to the swapchain VkFormat happens during the staging
// copy. Uploads leave images in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
// as the runtime requires at release.

#ifndef DOSBOX_XRVULKAN_H
#define DOSBOX_XRVULKAN_H

#include <cstdint>
#include <vector>

// gui/xr.h pulls in vulkan.h (when C_XR_VULKAN) ahead of the OpenXR
// platform header, so the Vk types used by XR_KHR_vulkan_enable are
// visible here.
#include "gui/xr.h"

#if C_XR_VULKAN

// Loader + core entry points present (no device yet). Logs when missing.
bool xrvk_available();

// Quiet loader probe for hot paths (no logging; result cached).
bool xrvk_have_loader();

// Device created and ready for uploads.
bool xrvk_is_ready();

// Create the Vulkan instance/device/queue for an XR session and fill
// the graphics binding (requirements are checked first). Safe to
// call redundantly (no-op when ready). False with a log line when
// Vulkan is missing or unusable.
bool xrvk_create(XrInstance xr_instance, XrSystemId xr_system,
                 XrGraphicsBindingVulkanKHR* binding_out);

// Tear down device/instance and unload the loader. Safe anytime.
void xrvk_shutdown();

// Pick a swapchain VkFormat from the runtime's format list (logs the
// choice) and remember it for uploads. Returns 0 on failure.
int64_t xrvk_pick_format(const int64_t* formats, const uint32_t count);

// Enumerate a swapchain's VkImages into neutral handles (resets
// their layout tracking to UNDEFINED).
bool xrvk_enumerate(XrSwapchain swapchain, const char* label,
                     std::vector<XrGfxImage>& out);

// Full-image copy of bottom-origin RGBA pixels (flips rows on
// upload) into one array layer. For exact-size overlay layers.
bool xrvk_upload_full(const uint64_t dst_handle, const int dst_layer,
                      const int w_px, const int h_px,
                      const uint32_t* rgba_bottom_up);

// Letterboxed blit of bottom-origin BGRA game pixels into a dst rect
// (top-left origin) of one array layer: clears the image to black
// first, GPU-scales when the runtime format supports blit-dst,
// CPU-scales otherwise. For eye layers.
bool xrvk_upload_fitted(const uint64_t dst_handle, const int dst_layer,
                         const int eye_w_px, const int eye_h_px,
                         const int dx_px, const int dy_px_top,
                         const int dw_px, const int dh_px,
                         const uint32_t* bgra_bottom_up,
                         const int src_w_px, const int src_h_px);

#endif // C_XR_VULKAN

#endif // DOSBOX_XRVULKAN_H
