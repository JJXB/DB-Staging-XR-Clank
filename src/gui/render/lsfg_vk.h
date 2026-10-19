// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// LSFG optical-flow dispatch (Vulkan compute).
//
// Runs the Lossless Scaling v3.1 frame-generation chain (mipmaps ->
// alpha -> beta -> gamma -> delta -> generate) from the user's cached
// precompiled SPIR-V blobs (see gui/render/lsfg.h). Pipeline facts
// (stage order, extents, formats, descriptor multisets, dispatch group
// math, uniform-buffer bytes, sampler params, barrier transitions) follow
// lsfg-vk v1.0.0 by PancakeTAS (MIT licensed); all Vulkan code here is an
// original implementation written for DOSBox-XR's CPU-frame in/out model:
// BGRX frames are swizzled to RGBX on upload, generated frames are read
// back to CPU and unswizzled, then flow through the normal present path.
//
// Any failure (no Vulkan loader, device/pipeline creation error, SPIR-V
// self-check mismatch) disables the GPU path with a one-time log and the
// caller falls back to CPU blend. Compiled only when C_XR_VULKAN (which
// guarantees the Vulkan headers); otherwise everything is blend.

#ifndef DOSBOX_LSFG_VK_H
#define DOSBOX_LSFG_VK_H

#include <cstdint>
#include <vector>

// Generate gen_count intermediates between prev and curr (tightly packed
// BGRX, top-down, w*h). Output timestamps are (i+1)/(gen_count+1),
// matching the desktop alpha ladder and XR divisor slots. Pixels for all
// gen_count frames are appended to frames_out (each w*h words).
// Returns false when the GPU path is unavailable (caller blends).
bool LSFGVK_Generate(const uint32_t* prev_px, const uint32_t* curr_px,
                     int w_px, int h_px, int gen_count,
                     std::vector<std::vector<uint32_t>>& frames_out);

// True once the GPU chain is up (for status lines).
bool LSFGVK_IsUp();

// Precision selector: false = fp16 blobs (base+49), true = fp32 blobs
// (base+98). Applied to chains built afterwards.
void LSFGVK_SetPreferFp32(bool prefer);

// Model selector: false = full quality v3.1, true = performance v3.1p.
// Applied to chains built afterwards.
void LSFGVK_SetPerfModel(bool perf);

// Flow working-resolution scale, as a percentage (25..100; 100 = full
// frame resolution). Lower shrinks the optical-flow pipeline. Applied to
// chains built afterwards (existing chains and UBOs are dropped).
void LSFGVK_SetFlowScale(int percent);

#endif // DOSBOX_LSFG_VK_H
