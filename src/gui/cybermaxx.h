// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// VictorMaxx CyberMaxx stereo input mode (see VMaxxCyMaxx.md, sections
// 7-8): scanline-interleaved decode. The headset's display board routes
// alternate incoming scanlines to the left vs. right TFT panel, so each
// eye's view arrives woven into even/odd rows of a single frame.

#ifndef DOSBOX_CYBERMAXX_H
#define DOSBOX_CYBERMAXX_H

#include <cstdint>
#include <vector>

// Extracts one half-height field per eye from a single
// scanline-interleaved frame: even rows drive one eye, odd rows the
// other (the GPU upscales them on upload, as with the generic
// 'interleaved' mode). A vertically line-doubled capture (VGA/SDL
// scaling duplicating every row) is detected per frame and
// de-doubled first, so both native and scaled captures decode to the
// same pair. Eye selection follows 'xr_eye_swap' ("first" eye is left
// unless swapped). The input frame is top-down; the eye buffers are
// written bottom-up (the uploader's glReadPixels-order convention).
// Both eye buffers must already be sized w*(h/2); mismatched or empty
// input is a no-op.
void CYBERMAXX_SubmitFrame(std::vector<uint32_t>& eye0,
                           std::vector<uint32_t>& eye1,
                           const std::vector<uint32_t>& frame, const int w,
                           const int h);

#endif
