// SPDX-FileCopyrightText:  2025-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Dedicated D3D12 device for the OpenXR D3D12 graphics binding
// (XR_KHR_D3D12_enable) plus a textured-quad compositor: CPU pixels
// go through a mapped upload-heap staging texture, then a fullscreen
// triangle draws them (viewport/scissor-fitted, linear sampling) into
// swapchain images used as render targets (hello_xr parity: no
// explicit swapchain barriers). The HLSL is compiled at runtime via
// D3DCompile, so no shader blobs ship and no build-time step exists.
// Swapchain textures come from the XR runtime, and the desktop window
// keeps rendering through the normal RenderBackend (the D3D12
// renderer when output=d3d12).
//
// d3d12.lib/dxgi.lib/d3dcompiler.lib are inbox system libraries
// (static import, no dynamic loading, no new dependencies).
//
// Pixel contract: all CPU buffers are bottom-up rows (matching the
// shared xr_raster builders and the xrstereo decode buffers).
// Rasters are RGBA byte order, game/eye frames BGRA byte order;
// conversion to the swapchain DXGI format happens during the staging
// fill. Uploads fence-wait before returning so images are safe to
// release.

#ifndef DOSBOX_XRD3D12_H
#define DOSBOX_XRD3D12_H

#include <cstdint>
#include <vector>

#include "gui/xr.h"

#if C_XR_D3D12

// Device+queue created and ready for uploads.
bool xrd12_is_ready();

// Create the D3D12 device/queue/resources for an XR session
// (LUID-matched adapter, runtime minimum feature level) and fill the
// graphics binding. Safe to call redundantly (no-op when ready).
// False with a log line when unusable.
bool xrd12_create(XrInstance xr_instance, XrSystemId xr_system,
                  XrGraphicsBindingD3D12KHR* binding_out);

// Tear down the device and all cached views/state. Safe anytime.
void xrd12_shutdown();

// Pick a swapchain DXGI_FORMAT from the runtime's format list (logs
// the choice) and remember it for uploads. Returns 0 on failure.
int64_t xrd12_pick_format(const int64_t* formats, const uint32_t count);

// Enumerate a swapchain's ID3D12Resource images into neutral handles.
bool xrd12_enumerate(XrSwapchain swapchain, const char* label,
                     std::vector<XrGfxImage>& out);

// Full-image draw of bottom-origin RGBA pixels (alpha-blended).
// For exact-size overlay layers.
bool xrd12_upload_full(const uint64_t dst_handle, const int dst_layer,
                       const int w_px, const int h_px,
                       const uint32_t* rgba_bottom_up);

// Letterboxed draw of bottom-origin BGRA game pixels into a dst rect
// (top-left origin): clears the image to black first, then draws the
// fitted quad (opaque). For eye layers.
bool xrd12_upload_fitted(const uint64_t dst_handle, const int dst_layer,
                         const int eye_w_px, const int eye_h_px,
                         const int dx_px, const int dy_px_top,
                         const int dw_px, const int dh_px,
                         const uint32_t* bgra_bottom_up,
                         const int src_w_px, const int src_h_px);

#endif // C_XR_D3D12

#endif // DOSBOX_XRD3D12_H
