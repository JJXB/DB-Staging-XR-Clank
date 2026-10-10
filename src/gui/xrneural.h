// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Neural depth estimation: ONNX Runtime loader (runtime-loaded, ABI
// mirror), hosted-model registry, background worker, depth-map cache,
// and the autodepth neural/fusion method queries used by stereo
// presentation and the menu.

#ifndef DOSBOX_XRNEURAL_H
#define DOSBOX_XRNEURAL_H

#include <cstdint>
#include <string>

#if C_OPENXR

// Neural autodepth method predicates (used by stereo presentation).
bool xr_autodepth_wants_neural(const std::string& method);
bool xr_autodepth_is_fusion(const std::string& method);

// Model selection (used by input shortcuts and the menu).
void        xr_neural_cycle_model(const int dir);
std::string openxr_menu_neural_model();

// Frame pipeline (used by stereo presentation).
void xr_neural_submit_frame(const uint32_t* frame, int w, int h);
void xr_neural_poll_missing_toast();
bool xr_neural_fill_map(float* depth_norm, int w, int h, bool& flat);

#endif // C_OPENXR

#endif
