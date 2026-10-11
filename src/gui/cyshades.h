// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// CyberShades stereo input mode (see cybershades.md): frame-sequential
// shutter-glasses decode with Chinon CyberShades signal polarity.

#ifndef DOSBOX_CYSHADES_H
#define DOSBOX_CYSHADES_H

#include <cstdint>
#include <vector>

// Routes one captured frame into the eye the game selected: the frame
// drives that eye's buffer, the other eye holding its last frame
// (same hold-last-frame behavior as the 'alternating' mode). Both eye
// buffers must already be sized to frame.size(); mismatched or empty
// input is a no-op. Eye selection follows 'xr_shutter_sync' (game
// shutter writes, else frame parity) with 'xr_eye_swap' on top.
void CYSHADES_SubmitFrame(std::vector<uint32_t>& eye0,
                          std::vector<uint32_t>& eye1,
                          const std::vector<uint32_t>& frame);

#endif
