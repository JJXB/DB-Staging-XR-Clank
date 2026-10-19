// SPDX-FileCopyrightText:  2025-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_XR_RASTER_H
#define DOSBOX_XR_RASTER_H

// Backend-neutral CPU rasterizers for the XR overlay textures
// (keyboard, notifications, menu, pointer-ray ribbon). Extracted
// verbatim from the OpenGL renderer's builders so both graphics
// backends paint identical pixels: the OpenGL renderer uploads the
// raster to a GL texture, the Vulkan path uploads it into a
// swapchain VkImage.
//
// Pixel contract: straight-alpha 0xAABBGGRR words (R,G,B,A byte
// order in memory), row 0 stored LAST (texture-bottom origin,
// matching GL upload semantics). Uploaders targeting top-left-origin
// images (Vulkan swapchains) flip rows on upload.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "dosbox_config.h"

#if C_OPENXR

// A key label for the overlay keyboard texture. Rect in texture
// pixels, y down from the top; `highlight` marks wide/special keys.
struct XrKeyLabel {
	const char* text;
	int         x0, y0, x1, y1;
	bool        highlight;
};

// One row of the in-headset menu. kind: 0=section header,
// 1=toggle, 2=slider, 3=list, 4=text field, 5=button, 6=save
// (field + SAVE button), 7=load (list + LOAD button). label is the
// row caption; value is the ON/OFF, slider/list value, or field
// content; action is the button caption for kinds 5/6/7.
// highlight outlines the hovered row; active brightens toggles.
//
// kind-0 rows open a section: each section is drawn as one bordered
// grid card (title row + its item rows) and the cards flow
// left-to-right, top-to-bottom across the panel.
struct XrMenuRow {
	int         kind;
	const char* label;
	const char* value;
	const char* action;
	bool        highlight;
	bool        active;
};

// Paints the overlay-keyboard raster (hover outline, pressed letter,
// uniform opacity for compositor blending). Returns false on bad
// input.
bool XrBuildKeyboardRaster(const XrKeyLabel* keys, const size_t num_keys,
                           const int tex_w_px, const int tex_h_px,
                           const int hover_index, const int pressed_index,
                           const float opacity,
                           std::vector<uint32_t>& img_out);

// Paints the notification-overlay raster from lines of text (newest
// first, up to 4 shown). opacity (0..1) scales every pixel's alpha,
// preserving the translucent-box ratio. Returns false on bad input.
bool XrBuildOsdRaster(const char* const* lines, size_t num_lines,
                      const int tex_w_px, const int tex_h_px,
                      const float opacity, std::vector<uint32_t>& img_out);

// Paints the menu-overlay raster from rows (same look as the desktop
// menu). Rows are grouped into section cards (kind-0 headers) laid out
// in a grid. cols_req is the requested column count: 0 selects the
// default (2, or 1 for a single section); 1..3 forces that many
// columns (clamped to the section count). font_mult scales the glyph
// size (1.0 = fit the row slot). Returns false when the rows cannot
// fit or on bad input.
bool XrBuildMenuRaster(const XrMenuRow* rows, const int num_rows,
                       const int tex_w_px, const int tex_h_px,
                       const float font_mult, const int cols_req,
                       std::vector<uint32_t>& img_out);

// Maps a panel-space point to a flat row index using the same grid
// geometry the raster draws. u is 0..1 left-to-right, v is 0..1
// top-to-bottom. cols_req must match the value passed to
// XrBuildMenuRaster (0 = default). On a hit, row_out is the flat row
// and mu_out is the horizontal fraction (0..1) within that row's card
// (for the slider left/right halves and the SAVE/LOAD button zone).
// Returns false when the point misses every interactive row (card
// gaps, the title bands, or outside the panel). Used by both the
// headset-ray and desktop-mouse hit-tests.
bool XrMenuGridHit(const XrMenuRow* rows, const int num_rows, const float u,
                   const float v, const int cols_req, int& row_out,
                   float& mu_out);

// Paints the pointer-ray ribbon gradient (bright core fading at the
// edges, straight alpha) at any size.
void XrBuildRayRaster(std::vector<uint32_t>& img_out, const int w_px,
                      const int h_px);

// Paints the radial input-mode wheel: `count` wedges around a hub,
// wedge 0 centred at the top and running clockwise (screen sense), with
// `highlight` painted brighter. Labels are drawn upright along each
// wedge's centre line. Straight alpha over a transparent background so
// the compositor blends it over the scene; opacity (0..1) scales every
// pixel's alpha. Returns false on bad input.
bool XrBuildWheelRaster(const char* const* labels, const int count,
                        const int highlight, const int tex_w_px,
                        const int tex_h_px, const float opacity,
                        std::vector<uint32_t>& img_out);

#endif // C_OPENXR

#endif // DOSBOX_XR_RASTER_H
