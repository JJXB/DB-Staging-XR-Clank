// SPDX-FileCopyrightText:  2025-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Backend-neutral CPU rasterizers for the XR overlays. Pixel output
// is identical to what the OpenGL renderer historically uploaded;
// see xr_raster.h for the pixel contract.

#include "xr_raster.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "dosbox_config.h"

#if C_OPENXR

#include "misc/support.h"

// Minimal 5x7 bitmap font for overlay key caps (X = ink, . = empty).
// Covers key labels only: digits, lowercase, comma/period, and the
// capitals used by the wide-key abbreviations (B, E, S).
struct XrFontGlyph {
	char        ch;
	const char* rows[7];
};

static const XrFontGlyph xr_font[] = {
        {'0', {"01110", "10001", "10011", "10101", "11001", "10001", "01110"}},
        {'1', {"00100", "01100", "00100", "00100", "00100", "00100", "01110"}},
        {'2', {"01110", "10001", "00001", "00110", "01000", "10000", "11111"}},
        {'3', {"11111", "00010", "00100", "00010", "00001", "10001", "01110"}},
        {'4', {"00010", "00110", "01010", "10010", "11111", "00010", "00010"}},
        {'5', {"11111", "10000", "11110", "00001", "00001", "10001", "01110"}},
        {'6', {"00110", "01000", "10000", "11110", "10001", "10001", "01110"}},
        {'7', {"11111", "00001", "00010", "00100", "01000", "01000", "01000"}},
        {'8', {"01110", "10001", "10001", "01110", "10001", "10001", "01110"}},
        {'9', {"01110", "10001", "10001", "01111", "00001", "00010", "01100"}},
        {'a', {"00000", "00000", "01110", "00001", "01111", "10001", "01111"}},
        {'b', {"10000", "10000", "10110", "11001", "10001", "10001", "10110"}},
        {'c', {"00000", "00000", "01110", "10000", "10000", "10001", "01110"}},
        {'d', {"00001", "00001", "01101", "11011", "10011", "10011", "01111"}},
        {'e', {"00000", "00000", "01110", "10001", "11111", "10000", "01110"}},
        {'f', {"00110", "01001", "01000", "11100", "01000", "01000", "01000"}},
        {'g', {"00000", "00000", "01111", "10001", "01111", "00001", "01110"}},
        {'h', {"10000", "10000", "10110", "11001", "10001", "10001", "10001"}},
        {'i', {"00100", "00000", "01100", "00100", "00100", "00100", "01110"}},
        {'j', {"00010", "00000", "00110", "00010", "00010", "10010", "01100"}},
        {'k', {"10000", "10000", "10010", "10100", "11000", "10100", "10010"}},
        {'l', {"01100", "00100", "00100", "00100", "00100", "00100", "01110"}},
        {'m', {"00000", "00000", "11010", "10101", "10101", "10101", "10101"}},
        {'n', {"00000", "00000", "10110", "11001", "10001", "10001", "10001"}},
        {'o', {"00000", "00000", "01110", "10001", "10001", "10001", "01110"}},
        {'p', {"00000", "00000", "10110", "11001", "10110", "10000", "10000"}},
        {'q', {"00000", "00000", "01101", "11011", "01111", "00001", "00001"}},
        {'r', {"00000", "00000", "10111", "11000", "10000", "10000", "10000"}},
        {'s', {"00000", "00000", "01111", "10000", "01110", "00001", "11110"}},
        {'t', {"01000", "01000", "11100", "01000", "01000", "01001", "00110"}},
        {'u', {"00000", "00000", "10001", "10001", "10001", "10001", "01110"}},
        {'v', {"00000", "00000", "10001", "10001", "10001", "01010", "00100"}},
        {'w', {"00000", "00000", "10001", "10001", "10101", "10101", "01010"}},
        {'x', {"00000", "00000", "10001", "10001", "01110", "10001", "10001"}},
        {'y', {"00000", "00000", "10001", "10001", "01111", "00001", "01110"}},
        {'z', {"00000", "00000", "11111", "00010", "00100", "01000", "11111"}},
        {',', {"00000", "00000", "00000", "00000", "01100", "00100", "01000"}},
        {'.', {"00000", "00000", "00000", "00000", "00000", "01100", "01100"}},
        {'/', {"00001", "00010", "00010", "00100", "01000", "01000", "10000"}},
        {'B', {"11110", "10001", "10001", "11110", "10001", "10001", "11110"}},
        {'E', {"11111", "10000", "10000", "11110", "10000", "10000", "11111"}},
        {'S', {"01111", "10000", "10000", "01110", "00001", "00001", "11110"}},
        {'C', {"01110", "10001", "10000", "10000", "10000", "10001", "01110"}},
        {'A', {"01110", "10001", "10001", "11111", "10001", "10001", "10001"}},
        {'T', {"11111", "00100", "00100", "00100", "00100", "00100", "00100"}},
        {'D', {"11110", "10001", "10001", "10001", "10001", "10001", "11110"}},
        {'F', {"11111", "10000", "10000", "11110", "10000", "10000", "10000"}},
        {'G', {"01110", "10001", "10000", "10111", "10001", "10001", "01111"}},
        {'H', {"10001", "10001", "10001", "11111", "10001", "10001", "10001"}},
        {'I', {"01110", "00100", "00100", "00100", "00100", "00100", "01110"}},
        {'J', {"00111", "00010", "00010", "00010", "00010", "10010", "01100"}},
        {'K', {"10001", "10010", "10100", "11000", "10100", "10010", "10001"}},
        {'L', {"10000", "10000", "10000", "10000", "10000", "10000", "11111"}},
        {'M', {"10001", "11011", "10101", "10101", "10001", "10001", "10001"}},
        {'N', {"10001", "11001", "10101", "10011", "10001", "10001", "10001"}},
        {'O', {"01110", "10001", "10001", "10001", "10001", "10001", "01110"}},
        {'P', {"11110", "10001", "10001", "11110", "10000", "10000", "10000"}},
        {'Q', {"01110", "10001", "10001", "10001", "10101", "10010", "01101"}},
        {'R', {"11110", "10001", "10001", "11110", "10100", "10010", "10001"}},
        {'U', {"10001", "10001", "10001", "10001", "10001", "10001", "01110"}},
        {'V', {"10001", "10001", "10001", "10001", "10001", "01010", "00100"}},
        {'W', {"10001", "10001", "10001", "10101", "10101", "11011", "10001"}},
        {'X', {"10001", "10001", "01010", "00100", "01010", "10001", "10001"}},
        {'Y', {"10001", "10001", "01010", "00100", "00100", "00100", "00100"}},
        {'Z', {"11111", "00001", "00010", "00100", "01000", "10000", "11111"}},
        {':', {"00000", "01100", "01100", "00000", "01100", "01100", "00000"}},
        {'-', {"00000", "00000", "00000", "11111", "00000", "00000", "00000"}},
        {'(', {"00010", "00100", "01000", "01000", "01000", "00100", "00010"}},
        {')', {"01000", "00100", "00010", "00010", "00010", "00100", "01000"}},
        {'+', {"00000", "00100", "00100", "11111", "00100", "00100", "00000"}},
        {'%', {"11001", "11010", "00010", "00100", "01000", "01011", "10011"}},
        {'=', {"00000", "00000", "11111", "00000", "11111", "00000", "00000"}},
        {'^', {"00100", "01110", "10101", "00100", "00100", "00000", "00000"}},
        {'<', {"00010", "00100", "01000", "10000", "01000", "00100", "00010"}},
        {'>', {"01000", "00100", "00010", "00001", "00010", "00100", "01000"}},
        {' ', {"00000", "00000", "00000", "00000", "00000", "00000", "00000"}},
        {'_', {"00000", "00000", "00000", "00000", "00000", "00000", "11111"}},
};

static const char* const* xr_find_glyph(const char c)
{
	for (const auto& g : xr_font) {
		if (g.ch == c) {
			return g.rows;
		}
	}
	return nullptr;
}

bool XrBuildKeyboardRaster(const XrKeyLabel* keys, const size_t num_keys,
                           const int tex_w_px, const int tex_h_px,
                           const int hover_index, const int pressed_index,
                           const float opacity,
                           std::vector<uint32_t>& img_out)
{
	if (!keys || num_keys == 0 || tex_w_px <= 0 || tex_h_px <= 0) {
		return false;
	}

	// Memory order R,G,B,A packed little-endian (0xAABBGGRR).
	// Mid-grey panel so the overlay reads against a black void.
	constexpr uint32_t BgColor      = 0xFF80726B;
	constexpr uint32_t KeyFill      = 0xFF382C26;
	constexpr uint32_t KeyFillWide  = 0xFF1A2F3A;
	constexpr uint32_t KeyBorder    = 0xFF6A5046;
	constexpr uint32_t KeyText      = 0xFFE6E6E6;
	constexpr uint32_t KeyTextWide  = 0xFFFFD479;
	constexpr uint32_t KeyTextPressed = 0xFF7FD4FF; // trigger-held letter

	std::vector<uint32_t> img(static_cast<size_t>(tex_w_px) *
	                                  static_cast<size_t>(tex_h_px),
	                          BgColor);

	auto set_px = [&](const int x, const int y, const uint32_t color) {
		if (x < 0 || y < 0 || x >= tex_w_px || y >= tex_h_px) {
			return;
		}
		// GL uploads memory row 0 as the texture bottom, while the
		// layout coords run y-down from the top: store flipped so
		// the keys read upright in the headset.
		img[(static_cast<size_t>(tex_h_px - 1 - y)) *
		            static_cast<size_t>(tex_w_px) +
		    static_cast<size_t>(x)] = color;
	};

	for (size_t k = 0; k < num_keys; ++k) {
		const auto& key = keys[k];
		if (key.x1 <= key.x0 || key.y1 <= key.y0) {
			continue;
		}
		const uint32_t fill = key.highlight ? KeyFillWide : KeyFill;
		// The pressed key (trigger held) lights its letter; hover
		// never touches letters, outline only.
		const uint32_t text =
		        (static_cast<int>(k) == pressed_index)
		                ? KeyTextPressed
		                : (key.highlight ? KeyTextWide : KeyText);
		for (int y = key.y0; y < key.y1; ++y) {
			for (int x = key.x0; x < key.x1; ++x) {
				set_px(x, y, fill);
			}
		}
		// 2px border.
		for (int x = key.x0; x < key.x1; ++x) {
			for (int b = 0; b < 2; ++b) {
				set_px(x, key.y0 + b, KeyBorder);
				set_px(x, key.y1 - 1 - b, KeyBorder);
			}
		}
		for (int y = key.y0; y < key.y1; ++y) {
			for (int b = 0; b < 2; ++b) {
				set_px(key.x0 + b, y, KeyBorder);
				set_px(key.x1 - 1 - b, y, KeyBorder);
			}
		}
		// Centered label.
		int len = 0;
		while (key.text[len] != '\0') {
			++len;
		}
		if (len > 0) {
			const int avail_w = (key.x1 - key.x0) - 16;
			const int avail_h = (key.y1 - key.y0) - 20;
			int scale         = avail_w / (6 * len - 1);
			const int vscale  = avail_h / 7;
			if (vscale < scale) {
				scale = vscale;
			}
			if (scale >= 1) {
				const int tw = scale * (6 * len - 1);
				const int th = scale * 7;
				int tx       = key.x0 + ((key.x1 - key.x0) - tw) / 2;
				const int ty = key.y0 + ((key.y1 - key.y0) - th) / 2;
				for (int ci = 0; ci < len; ++ci) {
					const char* const* rows = xr_find_glyph(
					        key.text[ci]);
					if (!rows) {
						tx += scale * 6;
						continue;
					}
					for (int gy = 0; gy < 7; ++gy) {
						for (int gx = 0; gx < 5; ++gx) {
							if (rows[gy][gx] != '1') {
								continue;
							}
							for (int sy = 0; sy < scale;
							     ++sy) {
								for (int sx = 0; sx < scale;
								     ++sx) {
									set_px(tx + gx * scale + sx,
									       ty + gy * scale + sy,
									       text);
								}
							}
						}
					}
					tx += scale * 6;
				}
			}
		}
	}
	// Bright outline around the hovered key (if any), drawn inside its
	// rect so neighbours are untouched.
	if (hover_index >= 0 &&
	    static_cast<size_t>(hover_index) < num_keys) {
		constexpr uint32_t KeyHover = 0xFF7FD4FF;
		const auto& hk = keys[hover_index];
		if (hk.x1 > hk.x0 && hk.y1 > hk.y0) {
			for (int x = hk.x0; x < hk.x1; ++x) {
				for (int b = 0; b < 3; ++b) {
					set_px(x, hk.y0 + b, KeyHover);
					set_px(x, hk.y1 - 1 - b, KeyHover);
				}
			}
			for (int y = hk.y0; y < hk.y1; ++y) {
				for (int b = 0; b < 3; ++b) {
					set_px(hk.x0 + b, y, KeyHover);
					set_px(hk.x1 - 1 - b, y, KeyHover);
				}
			}
		}
	}
	// Opacity for compositor blending (xr_keyboard_opacity): scale the
	// texture alpha uniformly; colors stay straight (non-premultiplied).
	if (opacity < 1.0f) {
		const uint32_t alpha =
		        static_cast<uint32_t>(opacity * 255.0f + 0.5f);
		for (auto& px : img) {
			px = (px & 0x00FFFFFFu) | (alpha << 24);
		}
	}

	size_t inked = 0;
	for (const auto px : img) {
		if (px != BgColor) {
			++inked;
		}
	}
	// One-time log; rebuilds happen on every hover change.
	static bool build_logged = false;
	if (!build_logged) {
		build_logged = true;
		LOG_MSG("XR: Built keyboard raster %dx%d (%zu key pixels)",
		        tex_w_px,
		        tex_h_px,
		        inked);
	}

	img_out = std::move(img);
	return true;
}

bool XrBuildOsdRaster(const char* const* lines, size_t num_lines,
                      const int tex_w_px, const int tex_h_px,
                      const float opacity, std::vector<uint32_t>& img_out)
{
	if (!lines || num_lines == 0 || tex_w_px <= 0 || tex_h_px <= 0) {
		return false;
	}
	if (num_lines > 4) {
		num_lines = 4;
	}

	// Straight-alpha RGBA: transparent background, opaque line boxes.
	constexpr uint32_t BgClear  = 0x00000000;
	constexpr uint32_t BoxFill   = 0xCC100C0A;
	constexpr uint32_t BoxBorder = 0xFF6A5046;
	constexpr uint32_t OsdText   = 0xFFE6E6E6;

	std::vector<uint32_t> img(static_cast<size_t>(tex_w_px) *
	                                  static_cast<size_t>(tex_h_px),
	                          BgClear);

	auto set_px = [&](const int x, const int y, const uint32_t color) {
		if (x < 0 || y < 0 || x >= tex_w_px || y >= tex_h_px) {
			return;
		}
		// Same bottom-origin storage as the keyboard rasterizer:
		// memory row 0 is the texture bottom, like the uploaded texture.
		img[(static_cast<size_t>(tex_h_px - 1 - y)) *
		            static_cast<size_t>(tex_w_px) +
		    static_cast<size_t>(x)] = color;
	};

	const int row_h = tex_h_px / static_cast<int>(num_lines);
	for (size_t li = 0; li < num_lines; ++li) {
		const int y0 = static_cast<int>(li) * row_h;
		const int y1 = y0 + row_h - 6; // gap between bars
		for (int y = y0; y < y1; ++y) {
			for (int x = 0; x < tex_w_px; ++x) {
				set_px(x, y, BoxFill);
			}
		}
		for (int x = 0; x < tex_w_px; ++x) {
			set_px(x, y0, BoxBorder);
			set_px(x, y0 + 1, BoxBorder);
			set_px(x, y1 - 1, BoxBorder);
			set_px(x, y1 - 2, BoxBorder);
		}

		int len = 0;
		while (lines[li][len] != '\0') {
			++len;
		}
		if (len == 0) {
			continue;
		}
		const int avail_w = tex_w_px - 32;
		const int avail_h = (y1 - y0) - 20;
		int scale         = avail_w / (6 * len - 1);
		const int vscale  = avail_h / 7;
		if (vscale < scale) {
			scale = vscale;
		}
		if (scale < 1) {
			continue;
		}
		const int tw = scale * (6 * len - 1);
		int tx       = 16;
		if (tw < avail_w) {
			tx += (avail_w - tw) / 2;
		}
		const int ty = y0 + ((y1 - y0) - scale * 7) / 2;
		for (int ci = 0; ci < len; ++ci) {
			const char* const* rows = xr_find_glyph(lines[li][ci]);
			if (rows) {
				for (int gy = 0; gy < 7; ++gy) {
					for (int gx = 0; gx < 5; ++gx) {
						if (rows[gy][gx] != '1') {
							continue;
						}
						for (int sy = 0; sy < scale; ++sy) {
							for (int sx = 0; sx < scale; ++sx) {
								set_px(tx + gx * scale + sx,
								       ty + gy * scale + sy,
								       OsdText);
							}
						}
					}
				}
			}
			tx += scale * 6;
		}
	}

	// Opacity for compositor blending (osd_xr_opacity): scale every
	// pixel's alpha, preserving the translucent-box ratio.
	float op = opacity;
	if (op < 0.0f) {
		op = 0.0f;
	}
	if (op > 1.0f) {
		op = 1.0f;
	}
	if (op < 1.0f) {
		const uint32_t mul =
		        static_cast<uint32_t>(op * 255.0f + 0.5f);
		for (auto& px : img) {
			px = (px & 0x00FFFFFFu) |
			     ((((px >> 24) * mul / 255) & 0xFFu) << 24);
		}
	}

	img_out = std::move(img);
	return true;
}

bool XrBuildWheelRaster(const char* const* labels, const int count,
                        const int highlight, const int tex_w_px,
                        const int tex_h_px, const float opacity,
                        std::vector<uint32_t>& img_out)
{
	if (!labels || count <= 0 || tex_w_px <= 0 || tex_h_px <= 0) {
		return false;
	}
	constexpr float Pi = 3.14159265358979f;

	// Memory order R,G,B,A packed little-endian (0xAABBGGRR).
	constexpr uint32_t BgClear    = 0x00000000;
	constexpr uint32_t WedgeFill  = 0xFF241B16;
	constexpr uint32_t WedgeHi    = 0xFFB4651A;
	constexpr uint32_t WedgeEdge  = 0xFF6A5046;
	constexpr uint32_t HubFill    = 0xFF14100E;
	constexpr uint32_t LabelInk   = 0xFFE6E6E6;
	constexpr uint32_t LabelInkHi = 0xFF201810;

	std::vector<uint32_t> img(static_cast<size_t>(tex_w_px) *
	                                  static_cast<size_t>(tex_h_px),
	                          BgClear);

	auto set_px = [&](const int x, const int y, const uint32_t color) {
		if (x < 0 || y < 0 || x >= tex_w_px || y >= tex_h_px) {
			return;
		}
		// Same bottom-origin storage as the other overlay rasters:
		// memory row 0 is the texture bottom, like the uploaded
		// texture.
		img[(static_cast<size_t>(tex_h_px - 1 - y)) *
		            static_cast<size_t>(tex_w_px) +
		    static_cast<size_t>(x)] = color;
	};

	const int   min_dim = std::min(tex_w_px, tex_h_px);
	const float cx      = tex_w_px * 0.5f;
	const float cy      = tex_h_px * 0.5f;
	const float outer   = 0.47f * static_cast<float>(min_dim);
	const float inner   = 0.27f * static_cast<float>(min_dim);
	const float step    = (2.0f * Pi) / static_cast<float>(count);
	const float start   = -Pi * 0.5f; // wedge 0 centred at the top

	// Sector body: each ring pixel picks its wedge by screen angle.
	for (int y = 0; y < tex_h_px; ++y) {
		const float dy = static_cast<float>(y) - cy;
		for (int x = 0; x < tex_w_px; ++x) {
			const float dx = static_cast<float>(x) - cx;
			const float r  = std::sqrt(dx * dx + dy * dy);
			if (r > outer || r < inner) {
				continue;
			}
			const float ang  = std::atan2(dy, dx);
			const float slot = (ang - start) / step;
			int idx          = static_cast<int>(std::lround(slot));
			idx = ((idx % count) + count) % count;
			uint32_t c = (idx == highlight) ? WedgeHi : WedgeFill;
			// Thin dark seam along the wedge boundaries (slot at
			// k+0.5); drawing on integer slots put the lines down
			// the wedge centres, misaligned with the selection.
			const float bdist = std::fabs(
			        (slot - 0.5f) -
			        static_cast<float>(std::lround(slot - 0.5f)));
			if (bdist * step < 0.02f) {
				c = WedgeEdge;
			}
			set_px(x, y, c);
		}
	}
	// Hub disc and both rim outlines.
	for (int y = 0; y < tex_h_px; ++y) {
		const float dy = static_cast<float>(y) - cy;
		for (int x = 0; x < tex_w_px; ++x) {
			const float dx = static_cast<float>(x) - cx;
			const float r  = std::sqrt(dx * dx + dy * dy);
			if (r <= inner) {
				set_px(x,
				       y,
				       (r > inner - 3.0f) ? WedgeEdge
				                          : HubFill);
			}
			if (r <= outer && r > outer - 3.0f) {
				set_px(x, y, WedgeEdge);
			}
		}
	}

	// Uniform glyph scale that fits every label in its wedge.
	const float rm  = (inner + outer) * 0.5f;
	const float arc = step * rm;
	int scale       = (static_cast<int>(0.42f * (outer - inner))) / 7;
	if (scale < 1) {
		scale = 1;
	}
	for (int i = 0; i < count; ++i) {
		int len = 0;
		while (labels[i][len] != '\0') {
			++len;
		}
		if (len <= 0) {
			continue;
		}
		const int wlim =
		        static_cast<int>(0.75f * arc) / (6 * len - 1);
		if (wlim < scale) {
			scale = wlim;
		}
	}
	if (scale < 1) {
		scale = 1;
	}

	for (int i = 0; i < count; ++i) {
		int len = 0;
		while (labels[i][len] != '\0') {
			++len;
		}
		if (len <= 0) {
			continue;
		}
		const float a  = start + static_cast<float>(i) * step;
		const float lx = cx + rm * std::cos(a);
		const float ly = cy + rm * std::sin(a);
		const int   tw = scale * (6 * len - 1);
		const int   th = scale * 7;
		int tx = static_cast<int>(lx) - tw / 2;
		const int ty = static_cast<int>(ly) - th / 2;
		const uint32_t ink =
		        (i == highlight) ? LabelInkHi : LabelInk;
		for (int ci = 0; ci < len; ++ci) {
			const char* const* rows = xr_find_glyph(labels[i][ci]);
			if (!rows) {
				tx += scale * 6;
				continue;
			}
			for (int gy = 0; gy < 7; ++gy) {
				for (int gx = 0; gx < 5; ++gx) {
					if (rows[gy][gx] != '1') {
						continue;
					}
					for (int sy = 0; sy < scale; ++sy) {
						for (int sx = 0; sx < scale;
						     ++sx) {
							set_px(tx + gx * scale +
							               sx,
							       ty + gy * scale +
							               sy,
							       ink);
						}
					}
				}
			}
			tx += scale * 6;
		}
	}

	// Opacity for compositor blending: scale every pixel's alpha.
	float op = opacity;
	if (op < 0.0f) {
		op = 0.0f;
	}
	if (op > 1.0f) {
		op = 1.0f;
	}
	if (op < 1.0f) {
		const uint32_t mul =
		        static_cast<uint32_t>(op * 255.0f + 0.5f);
		for (auto& px : img) {
			px = (px & 0x00FFFFFFu) |
			     ((((px >> 24) * mul / 255) & 0xFFu) << 24);
		}
	}

	img_out = std::move(img);
	return true;
}

void XrBuildRayRaster(std::vector<uint32_t>& img_out, const int w_px,
                      const int h_px)
{
	if (w_px <= 0 || h_px <= 0) {
		img_out.clear();
		return;
	}
	// Memory order R,G,B,A packed little-endian (0xAABBGGRR).
	// Cyan-white core with soft side edges and a fade at both ends
	// (U runs along the ribbon); straight alpha for compositor
	// blending.
	std::vector<uint32_t> img(static_cast<size_t>(w_px) * h_px, 0);
	for (int y = 0; y < h_px; ++y) {
		const float v  = (y + 0.5f) / h_px * 2.0f - 1.0f; // -1..1
		const float ev = std::max(0.0f, 1.0f - v * v);
		for (int x = 0; x < w_px; ++x) {
			const float u  = (x + 0.5f) / w_px;
			const float eu = std::min(1.0f,
			                          std::min(u, 1.0f - u) * 12.0f);
			const float a = ev * std::max(0.0f, eu);
			const uint32_t alpha =
			        static_cast<uint32_t>(190.0f * a + 0.5f);
			img[static_cast<size_t>(y) * w_px + x] =
			        (alpha << 24) | 0x00FFD47Fu; // BGR: 7F D4 FF
		}
	}
	img_out = std::move(img);
}

// ---------------------------------------------------------------------------
// Menu section grid (layout shared by the raster and the hit-test)
// ---------------------------------------------------------------------------
// kind-0 rows open sections: each section is one bordered card (a
// title band plus its item rows). Cards flow left-to-right and then
// top-to-bottom across a fixed column count (1 section = 1 column,
// otherwise 2). Every rect below is in normalized 0..1 panel space so
// the raster (pixels) and the hit-test (UV) derive identical geometry.
struct XrMenuCell {
	int first_row = 0; // flat index of the section header row
	int num_rows  = 0; // header row + item rows
	int col       = 0; // 0-based grid column
	int grid_row  = 0; // 0-based grid row
};

// Groups the flat rows into cards. Rows before the first kind-0 header
// (including that leading header) form the first card. want_cols is
// the requested column count: 0 selects the default (1 for a single
// section, else 2); a positive value forces that count (clamped to the
// number of sections). Returns false on bad input.
static bool xr_menu_layout_cells(const XrMenuRow* rows, const int num_rows,
                                 const int want_cols,
                                 std::vector<XrMenuCell>& cells_out,
                                 int& cols_out, int& grid_rows_out)
{
	cells_out.clear();
	if (!rows || num_rows <= 0) {
		return false;
	}
	int start = 0;
	for (int i = 1; i <= num_rows; ++i) {
		if (i == num_rows || rows[i].kind == 0) {
			cells_out.push_back({start, i - start, 0, 0});
			start = i;
		}
	}
	const int n = static_cast<int>(cells_out.size());
	int cols    = (want_cols >= 1) ? want_cols : ((n <= 1) ? 1 : 2);
	if (cols > n) {
		cols = n;
	}
	const int grid_rows = (n + cols - 1) / cols;
	for (int i = 0; i < n; ++i) {
		cells_out[i].col      = i % cols;
		cells_out[i].grid_row = i / cols;
	}
	cols_out      = cols;
	grid_rows_out = grid_rows;
	return true;
}

struct XrMenuGridGeom {
	int   cols      = 1;
	int   grid_rows = 1;
	float gap       = 0.012f; // margin between and around cards
	float pad       = 0.008f; // inner card margin
	float cell_w    = 1.0f;
	float cell_h    = 1.0f;
	float max_slot  = 0.11f; // cap on a row slot's height
};

static XrMenuGridGeom xr_menu_grid_geom(const int cols, const int grid_rows)
{
	XrMenuGridGeom g;
	g.cols      = (cols > 0) ? cols : 1;
	g.grid_rows = (grid_rows > 0) ? grid_rows : 1;
	g.cell_w    = (1.0f - g.gap * (g.cols + 1)) / g.cols;
	g.cell_h    = (1.0f - g.gap * (g.grid_rows + 1)) / g.grid_rows;
	return g;
}

static void xr_menu_cell_rect(const XrMenuGridGeom& g, const int col,
                              const int grid_row, float& x0, float& y0,
                              float& x1, float& y1)
{
	x0 = g.gap + col * (g.cell_w + g.gap);
	y0 = g.gap + grid_row * (g.cell_h + g.gap);
	x1 = x0 + g.cell_w;
	y1 = y0 + g.cell_h;
}

// Vertical slot layout inside a card (slot 0 is the title). Fills the
// first slot's top and the slot height, centred in the card.
static void xr_menu_slots(const XrMenuGridGeom& g, const float y0,
                          const float y1, const int nrows, float& first_y0,
                          float& slot_h)
{
	const int slots = (nrows > 0) ? nrows : 1;
	float sh        = ((y1 - y0) - 2.0f * g.pad) / slots;
	if (sh > g.max_slot) {
		sh = g.max_slot;
	}
	if (sh < 0.006f) {
		sh = 0.006f;
	}
	first_y0 = y0 + ((y1 - y0) - sh * slots) * 0.5f;
	slot_h   = sh;
}

// Shared menu rasterizer feeding the VR menu texture, the desktop-menu
// composite, and the Vulkan menu layer (same look, separate pixels):
// paints the rows into section-card cells in img (memory order R,G,B,A
// packed little-endian 0xAABBGGRR, like the keyboard/OSD builders;
// row 0 is stored last). Returns false when the rows cannot fit.
bool XrBuildMenuRaster(const XrMenuRow* rows, const int num_rows,
                       const int tex_w_px, const int tex_h_px,
                       float font_mult, const int cols_req,
                       std::vector<uint32_t>& img)
{
	if (!rows || num_rows <= 0 || tex_w_px <= 0 || tex_h_px <= 0) {
		return false;
	}
	// Dark panel, light text, cyan headers, amber highlight/active.
	constexpr uint32_t BgPanel  = 0xF2261C14;
	constexpr uint32_t BgCard   = 0xFF2C2620;
	constexpr uint32_t Border   = 0xFF46506A;
	constexpr uint32_t MenuText = 0xFFE6E6E6;
	constexpr uint32_t DimText  = 0xFF868F9A;
	constexpr uint32_t Header   = 0xFF79D4FF;
	constexpr uint32_t Accent   = 0xFFFFD47F;

	std::vector<XrMenuCell> cells;
	int cols = 1, grid_rows = 1;
	if (!xr_menu_layout_cells(rows, num_rows, cols_req, cells, cols,
	                          grid_rows)) {
		return false;
	}

	img.assign(static_cast<size_t>(tex_w_px) * tex_h_px, BgPanel);
	auto set_px = [&](int x, int y, uint32_t c) {
		if (x < 0 || y < 0 || x >= tex_w_px || y >= tex_h_px) {
			return;
		}
		// Flip: raster rows are top-down, texture row 0 is bottom.
		img[static_cast<size_t>(tex_h_px - 1 - y) * tex_w_px + x] = c;
	};
	auto fill_box = [&](int x0, int y0, int x1, int y1, uint32_t c) {
		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				set_px(x, y, c);
			}
		}
	};
	auto hollow_box = [&](int x0, int y0, int x1, int y1, uint32_t c) {
		for (int x = x0; x < x1; ++x) {
			set_px(x, y0, c);
			set_px(x, y1 - 1, c);
		}
		for (int y = y0; y < y1; ++y) {
			set_px(x0, y, c);
			set_px(x1 - 1, y, c);
		}
	};
	auto text_w = [](const char* s, int scale) {
		int n = 0;
		for (const char* p = s; p && *p; ++p) {
			++n;
		}
		return n ? scale * (6 * n - 1) : 0;
	};
	auto draw_text = [&](const char* s, int tx, int ty, int scale,
	                     uint32_t color) {
		for (const char* p = s; p && *p; ++p) {
			const char* const* glyph = xr_find_glyph(*p);
			if (!glyph) {
				tx += scale * 6; // missing glyph: blank advance
				continue;
			}
			for (int gy = 0; gy < 7; ++gy) {
				for (int gx = 0; gx < 5; ++gx) {
					if (glyph[gy][gx] != '1') {
						continue;
					}
					for (int sy = 0; sy < scale; ++sy) {
						for (int sx = 0; sx < scale;
						     ++sx) {
							set_px(tx + gx * scale + sx,
							       ty + gy * scale + sy,
							       color);
						}
					}
				}
			}
			tx += scale * 6;
		}
	};

	const XrMenuGridGeom g = xr_menu_grid_geom(cols, grid_rows);
	if (font_mult < 0.5f) {
		font_mult = 0.5f;
	}
	if (font_mult > 2.0f) {
		font_mult = 2.0f;
	}
	auto px_x = [&](const float x) {
		return static_cast<int>(x * tex_w_px + 0.5f);
	};
	auto px_y = [&](const float y) {
		return static_cast<int>(y * tex_h_px + 0.5f);
	};
	// One uniform font scale for every card, sized to the densest
	// section (so the grid reads evenly rather than per-card).
	int max_cell_rows = 1;
	for (const auto& c : cells) {
		if (c.num_rows > max_cell_rows) {
			max_cell_rows = c.num_rows;
		}
	}
	float uniform_sh = (g.cell_h - 2.0f * g.pad) / max_cell_rows;
	if (uniform_sh > g.max_slot) {
		uniform_sh = g.max_slot;
	}
	const int uniform_slot_px = std::max(7, px_y(uniform_sh));
	int uniform_scale =
	        static_cast<int>((uniform_slot_px - 10) * font_mult / 7.0f);
	const int uniform_max = (uniform_slot_px - 2) / 7;
	if (uniform_scale < 1) {
		uniform_scale = 1;
	}
	if (uniform_max >= 1 && uniform_scale > uniform_max) {
		uniform_scale = uniform_max;
	}
	if (uniform_scale < 1) {
		uniform_scale = 1;
	}

	for (const auto& cell : cells) {
		float x0n = 0.0f, y0n = 0.0f, x1n = 0.0f, y1n = 0.0f;
		xr_menu_cell_rect(g, cell.col, cell.grid_row, x0n, y0n, x1n, y1n);
		const int cx0 = px_x(x0n);
		const int cy0 = px_y(y0n);
		const int cx1 = px_x(x1n);
		const int cy1 = px_y(y1n);
		if (cx1 <= cx0 || cy1 <= cy0) {
			continue;
		}
		// Card background + border.
		fill_box(cx0, cy0, cx1, cy1, BgCard);
		hollow_box(cx0, cy0, cx1, cy1, Border);

		const int inset = std::max(4, px_x(g.gap * 0.6f));
		const int tx0   = cx0 + inset;
		const int tx1   = cx1 - inset;

		float fy0 = 0.0f, sh = 0.0f;
		xr_menu_slots(g, y0n, y1n, cell.num_rows, fy0, sh);
		const int slot_px = std::max(7, px_y(sh));
		const int scale   = uniform_scale;

		for (int k = 0; k < cell.num_rows; ++k) {
			const XrMenuRow& r = rows[cell.first_row + k];
			const int ry0      = px_y(fy0 + k * sh);
			const int ry1      = ry0 + slot_px;
			const int ty = ry0 + (slot_px - 7 * scale) / 2;

			// Hovered row gets a cyan outline inside the card.
			if (r.highlight) {
				hollow_box(cx0 + 2, ry0 + 1, cx1 - 2, ry1 - 1,
				           Accent);
			}

			if (k == 0) { // title: centred cyan text + rule
				const int tw = text_w(r.label, scale);
				draw_text(r.label,
				          cx0 + (cx1 - cx0 - tw) / 2,
				          ty,
				          scale,
				          Header);
				fill_box(tx0, ry1 - 1, tx1, ry1, Border);
				continue;
			}

			switch (r.kind) {
			case 5: { // centered button: [ label ]
				const std::string buf =
				        std::string("[ ") +
				        (r.label ? r.label : "") + " ]";
				const int tw = text_w(buf.c_str(), scale);
				const int tx = cx0 + (cx1 - cx0 - tw) / 2;
				const uint32_t hot = r.highlight ? Header
				                                 : DimText;
				hollow_box(tx - 8, ry0 + 3, tx + tw + 8,
				           ry1 - 3, hot);
				draw_text(buf.c_str(), tx, ty, scale,
				          r.highlight ? Header : MenuText);
				break;
			}
			default: {
				// Left caption.
				if (r.label && *r.label) {
					draw_text(r.label, tx0, ty, scale,
					          MenuText);
				}
				if (r.kind == 1) { // toggle: right value
					const char* v = (r.value && *r.value)
					                        ? r.value
					                        : "OFF";
					const uint32_t c = r.active ? Accent
					                            : DimText;
					const int tw = text_w(v, scale);
					draw_text(v, tx1 - tw, ty, scale, c);
				} else if (r.kind == 2 || r.kind == 3) {
					// slider/list: right-aligned
					// "< value >"; arrows amber on
					// hover like buttons.
					const uint32_t arrow = r.highlight
					                               ? Header
					                               : DimText;
					const char* v =
					        (r.value && *r.value) ? r.value
					                              : "";
					const int vw = text_w(v, scale);
					const int aw = text_w("<", scale);
					const int gp = 2 * scale;
					int tx = tx1 - text_w(">", scale);
					draw_text(">", tx, ty, scale, arrow);
					tx -= gp + vw;
					draw_text(v, tx, ty, scale, MenuText);
					tx -= gp + aw;
					draw_text("<", tx, ty, scale, arrow);
				} else if (r.kind == 4) {
					// text field: value after caption.
					const int lw = text_w(
					        r.label ? r.label : "", scale);
					const char* v = (r.value && *r.value)
					                        ? r.value
					                        : "_";
					const int fx0 = tx0 + lw + 3 * scale;
					const int fx1 = tx1;
					if (fx1 > fx0) {
						hollow_box(fx0 - 4, ry0 + 3,
						           fx1 + 4, ry1 - 3,
						           r.highlight
						                   ? Accent
						                   : Border);
						// Clip to the field width.
						char clip[64];
						int maxc =
						        (fx1 - fx0) /
						        (6 * scale);
						if (maxc < 1) {
							maxc = 1;
						}
						if (maxc > 63) {
							maxc = 63;
						}
						int ci = 0;
						for (const char* p = v;
						     p && *p && ci < maxc;
						     ++p, ++ci) {
							clip[ci] = *p;
						}
						clip[ci] = 0;
						draw_text(clip, fx0, ty,
						          scale, MenuText);
					}
				} else if (r.kind == 6 || r.kind == 7) {
					// save/load: right [ action ]
					// button, amber on hover.
					const std::string buf =
					        std::string("[ ") +
					        (r.action ? r.action : "") +
					        " ]";
					const int tw = text_w(buf.c_str(),
					                      scale);
					const int tx = tx1 - tw;
					const uint32_t hot =
					        r.highlight ? Header : DimText;
					hollow_box(tx - 8, ry0 + 3,
					           tx + tw + 8, ry1 - 3, hot);
					draw_text(buf.c_str(), tx, ty,
					          scale,
					          r.highlight ? Header
					                          : MenuText);
					// Left content: "File: name"
					// or "< preset >".
					std::string left;
					if (r.kind == 6) {
						left = std::string("File: ") +
						       ((r.value && *r.value)
						                ? r.value
						                : "_");
					} else {
						left = std::string("< ") +
						       ((r.value && *r.value)
						                ? r.value
						                : "") +
						       " >";
					}
					const int lw = text_w(
					        r.label ? r.label : "", scale);
					draw_text(left.c_str(),
					          tx0 + lw + 3 * scale, ty,
					          scale, MenuText);
				}
				break;
			}
			}
		}
	}
	return true;
}

// Maps a panel-space point to a flat row index using the grid geometry
// the raster draws. See the header for the coordinate contract.
bool XrMenuGridHit(const XrMenuRow* rows, const int num_rows, const float u,
                   const float v, const int cols_req, int& row_out,
                   float& mu_out)
{
	row_out = -1;
	mu_out  = 0.5f;
	if (!rows || num_rows <= 0) {
		return false;
	}
	std::vector<XrMenuCell> cells;
	int cols = 1, grid_rows = 1;
	if (!xr_menu_layout_cells(rows, num_rows, cols_req, cells, cols,
	                          grid_rows)) {
		return false;
	}
	const XrMenuGridGeom g = xr_menu_grid_geom(cols, grid_rows);

	const int col = static_cast<int>((u - g.gap) / (g.cell_w + g.gap));
	const int gr  = static_cast<int>((v - g.gap) / (g.cell_h + g.gap));
	if (col < 0 || gr < 0 || col >= cols || gr >= grid_rows) {
		return false;
	}
	const XrMenuCell* cell = nullptr;
	for (const auto& c : cells) {
		if (c.col == col && c.grid_row == gr) {
			cell = &c;
			break;
		}
	}
	if (!cell) {
		return false;
	}
	float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
	xr_menu_cell_rect(g, col, gr, x0, y0, x1, y1);
	if (u < x0 || u >= x1 || v < y0 || v >= y1) {
		return false;
	}
	float fy0 = 0.0f, sh = 0.0f;
	xr_menu_slots(g, y0, y1, cell->num_rows, fy0, sh);
	int k = static_cast<int>((v - fy0) / sh);
	if (k < 0) {
		k = 0;
	}
	if (k >= cell->num_rows) {
		k = cell->num_rows - 1;
	}
	if (k == 0) { // title band is not interactive
		return false;
	}
	row_out = cell->first_row + k;
	mu_out  = (u - x0) / g.cell_w;
	if (mu_out < 0.0f) {
		mu_out = 0.0f;
	}
	if (mu_out > 1.0f) {
		mu_out = 1.0f;
	}
	return true;
}

#endif // C_OPENXR
