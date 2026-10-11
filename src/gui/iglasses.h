// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Virtual i-O i-glasses! stereo input mode (see IGLASSES.md, sections
// 8-9): scanline-interleaved decode. The headset's display hardware
// splits one non-interlaced frame buffer into two eye views on
// alternate scanlines (first scanline = left eye: 320x400 becomes two
// 320x200 views), so each eye's view arrives woven into even/odd rows
// of a single frame.

#ifndef DOSBOX_IGLASSES_H
#define DOSBOX_IGLASSES_H

#include <cstdint>
#include <vector>

// Builds one full-height bob-deinterlaced view per eye from a single
// scanline-interleaved frame: even rows drive one eye, odd rows the
// other, each row duplicated in place so both eyes stay full-frame and
// self-consistent (no within-eye doubling). Eye selection follows
// 'xr_eye_swap' ("first" eye is left unless swapped). Both eye buffers
// must already be sized w*h; mismatched or empty input is a no-op.
void IGLASSES_SubmitFrame(std::vector<uint32_t>& eye0,
                          std::vector<uint32_t>& eye1,
                          const std::vector<uint32_t>& frame, const int w,
                          const int h);

#endif
