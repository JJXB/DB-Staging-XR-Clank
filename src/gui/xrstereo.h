// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Stereo image handling: eye buffers, depth-view synthesis, autodepth
// estimation, Voodoo depth capture, and the headset/mirror stereo
// blit (OPENXR_BlitMirrorToSwapchain).

#ifndef DOSBOX_XRSTEREO_H
#define DOSBOX_XRSTEREO_H

#include <cstdint>

#if C_OPENXR

// Mirror/swapchain stereo blit (called from the OpenGL renderer and
// the SDL frame path; forward-declared locally there too).
void OPENXR_BlitMirrorToSwapchain();

// Voodoo depth capture (called from the Voodoo emulation).
void XR_VoodooDepthFrame(const uint32_t width, const uint32_t height,
                         const uint32_t* frame_argb);

// Shared eye buffers (owned here, filled by capture paths).
extern bool sdl_xr_depth_valid;

#endif // C_OPENXR

#endif
