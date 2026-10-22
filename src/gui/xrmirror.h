// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Optional desktop "headset mirror" window: a second SDL window that
// shows the two eye images submitted to the headset side by side.

#ifndef DOSBOX_XRMIRROR_H
#define DOSBOX_XRMIRROR_H

#include <cstdint>

#include "dosbox.h"

#if C_OPENXR
#include <SDL3/SDL.h>

// Update (or close) the mirror window. eye_left/eye_right are 32-bit
// 0xAARRGGBB pixels in bottom-up row order (the same buffers uploaded
// to the headset); pass null to only service the config switch.
void XR_MirrorWindowUpdate(const uint32_t* eye_left,
                           const uint32_t* eye_right, int w_px, int h_px);

// Returns true when the event belongs to the mirror window (including
// its close request), so the main-window handler must skip it.
bool XR_MirrorWindowHandleEvent(const SDL_Event& event);

// Destroy the mirror window and renderer (safe to call repeatedly).
void XR_MirrorWindowShutdown();

#endif // C_OPENXR

#endif
