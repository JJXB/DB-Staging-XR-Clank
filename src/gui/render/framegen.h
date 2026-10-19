// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Backend-agnostic frame generation (interpolation).
//
// DOSBox emulates final VGA pixels only: there are no motion vectors or
// depth buffers, so vendor frame-gen that needs them (FSR3 FG, XeSS-FG)
// cannot run here. What CAN run is color-only interpolation between the
// last two emulated frames: this module keeps that two-frame history and
// blends intermediates (alpha 0 = previous, 1 = current).
//
// The same switch pair drives both outputs from one place
// (GFX_MaybePresentFrame in gui/sdl_gui.cpp):
//   - desktop: extra interpolated presents per emulated frame
//     ([framegen] framegen_desktop + framegen_desktop_extra).
//   - headset: divisor-skipped XR slots are submitted as blends instead
//     of skipped ([framegen] framegen_xr, needs xr_submit_divisor >= 2
//     to have slots to fill).
//
// True optical-flow LSFG (Lossless Scaling shaders extracted from the
// user's Lossless.dll) remains a possible future drop-in behind this
// same interface: FRAMEGEN_Blend is where that dispatcher would plug in.
// The proprietary Lossless.dll itself exposes no frame-feed API (only
// app-control exports), so it cannot be bound directly.

#ifndef DOSBOX_FRAMEGEN_H
#define DOSBOX_FRAMEGEN_H

#include <cstdint>
#include <vector>

class SectionProp;

SectionProp* get_framegen_section();

// Register the [framegen] config section (called from GFX_AddConfigSection).
void FRAMEGEN_InitSection(SectionProp& section);

// Record one emulated frame (tightly packed BGRX words, top-down rows).
// A size change resets the pair (no blend across mode switches).
void FRAMEGEN_PushFrame(const uint32_t* pixels, int w_px, int h_px);

// True once two same-size frames are available.
bool FRAMEGEN_HavePair(int& w_px_out, int& h_px_out);

// Blend previous/current into out (tightly packed BGRX, top-down).
// Returns false when no pair is available yet.
bool FRAMEGEN_Blend(float alpha_0_to_1,
                    std::vector<uint32_t>& pixels_out,
                    int& w_px_out, int& h_px_out);

// Live [framegen] switches (all changeable while running).
bool FRAMEGEN_DesktopEnabled();
bool FRAMEGEN_XrEnabled();

// Extra desktop presents per emulated frame (1..3, default 1).
// Presented alphas form a ladder: 1/(extra+1) .. extra/(extra+1).
int FRAMEGEN_DesktopExtra();

// One-shot LSFG shader staging for framegen_method=lsfg (extract once,
// cache, re-extract on DLL change). Cheap after the first call; call it
// every present tick. No-op unless the method is lsfg and at least one
// output switch is on.
void FRAMEGEN_MaybeEnsureLsfg();

// True when framegen_method=lsfg (shaders wanted; use the GPU batch path
// below when it delivers, CPU blend otherwise).
bool FRAMEGEN_MethodIsLsfg();

// Batch-generate gen_count optical-flow intermediates for the current
// history pair (timestamps (i+1)/(gen_count+1), matching the desktop
// ladder and XR divisor slots). Appends gen_count frames on success.
// Returns 0 when the GPU path is unavailable (caller uses CPU blend).
int FRAMEGEN_GenerateLsfg(int gen_count,
                           std::vector<std::vector<uint32_t>>& frames_out,
                           int& w_px_out, int& h_px_out);

#endif // DOSBOX_FRAMEGEN_H
