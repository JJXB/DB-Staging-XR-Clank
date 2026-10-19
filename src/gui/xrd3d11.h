// SPDX-FileCopyrightText:  2025-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Dedicated D3D11 device for the OpenXR D3D11 graphics binding
// (XR_KHR_D3D11_enable) plus a shader-free compositor: CPU pixels go
// through UpdateSubresource into a staging texture, then
// CopySubresourceRegion (exact-size overlays) or the video processor
// scaler (letterboxed eye layers; CPU bilinear fallback when the
// processor is unavailable). No shaders, no window/swapchain of our
// own - swapchain textures come from the XR runtime, and the desktop
// window keeps rendering through the normal RenderBackend (the D3D11
// renderer when output=d3d11).
//
// d3d11.dll/dxgi.dll are inbox system libraries (static import, no
// dynamic loading, no new dependencies).
//
// Pixel contract: all CPU buffers are bottom-up rows (matching the
// shared xr_raster builders and the xrstereo decode buffers).
// Rasters are RGBA byte order, game/eye frames BGRA byte order;
// conversion to the swapchain DXGI format happens during the staging
// fill. Uploads Flush() the immediate context before returning.

#ifndef DOSBOX_XRD3D11_H
#define DOSBOX_XRD3D11_H

#include <cstdint>
#include <vector>

#include "gui/xr.h"

#if C_XR_D3D11

// Device created and ready for uploads.
bool xrd11_is_ready();

// Create the D3D11 device/context for an XR session (LUID-matched
// adapter, runtime minimum feature level) and fill the graphics
// binding. Safe to call redundantly (no-op when ready). False with a
// log line when unusable.
bool xrd11_create(XrInstance xr_instance, XrSystemId xr_system,
                  XrGraphicsBindingD3D11KHR* binding_out);

// Tear down the device and all views/caches. Safe anytime.
void xrd11_shutdown();

// Pick a swapchain DXGI_FORMAT from the runtime's format list (logs
// the choice) and remember it for uploads. Returns 0 on failure.
int64_t xrd11_pick_format(const int64_t* formats, const uint32_t count);

// Enumerate a swapchain's ID3D11Texture2D images into neutral handles.
bool xrd11_enumerate(XrSwapchain swapchain, const char* label,
                     std::vector<XrGfxImage>& out);

// Full-texture copy of bottom-origin RGBA pixels. For exact-size
// overlay layers.
bool xrd11_upload_full(const uint64_t dst_handle, const int dst_layer,
                       const int w_px, const int h_px,
                       const uint32_t* rgba_bottom_up);

// Letterboxed scale of bottom-origin BGRA game pixels into a dst rect
// (top-left origin) of one array layer: clears the image to black
// first, then scales via the video processor (CPU bilinear fallback).
// For eye layers.
bool xrd11_upload_fitted(const uint64_t dst_handle, const int dst_layer,
                         const int eye_w_px, const int eye_h_px,
                         const int dx_px, const int dy_px_top,
                         const int dw_px, const int dh_px,
                         const uint32_t* bgra_bottom_up,
                         const int src_w_px, const int src_h_px);

#endif // C_XR_D3D11

#endif // DOSBOX_XRD3D11_H
