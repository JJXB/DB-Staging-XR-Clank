// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gui/render/framegen.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>

#include "config/setup.h"
#include "gui/render/lsfg.h"
#include "gui/render/lsfg_vk.h"
#include "misc/logging.h"

SectionProp* get_framegen_section()
{
	auto section = get_section("framegen");
	assert(section);
	return section;
}

void FRAMEGEN_InitSection(SectionProp& section)
{
	using enum Property::Changeable::Value;

	auto pbool_desk = section.AddBool("framegen_desktop", Always, false);
	pbool_desk->SetHelp(
	        "Generate interpolated frames for the desktop window (off by\n"
	        "default). Each emulated frame is followed by 'framegen_desktop_extra'\n"
	        "blended presents, smoothing motion on high-refresh displays.\n"
	        "Works on every [sdl] 'output' backend (opengl, texture,\n"
	        "vulkan, d3d11, d3d12).\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.\n"
	        "Skipped while capturing so screenshots stay pixel-exact.");

	auto pbool_xr = section.AddBool("framegen_xr", Always, false);
	pbool_xr->SetHelp(
	        "Generate interpolated frames for the VR headset (off by default).\n"
	        "Divisor-skipped XR slots are submitted as blends instead of being\n"
	        "skipped, so the headset keeps its cadence (e.g. emulation at 35fps\n"
	        "with 'xr_submit_divisor = 2' submits 35Hz: real, blend, real,\n"
	        "blend, ...). Needs an XR-capable backend ('output=opengl',\n"
	        "'output=vulkan', 'output=d3d11', or 'output=d3d12') plus\n"
	        "'xr_submit_divisor' >= 2 to have slots to fill (at divisor 1\n"
	        "every frame is already submitted, so there is nothing to fill).\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.\n"
	        "The blended submits also mirror to the desktop window (shared\n"
	        "present path).");

	auto pint_extra = section.AddInt("framegen_desktop_extra", Always, 1);
	pint_extra->SetHelp(
	        "Extra desktop presents per emulated frame when 'framegen_desktop'\n"
	        "is on (1 by default: 2x present rate with a 50%% blend; 2 gives\n"
	        "a 33%%/66%% ladder, 3 a 25%%/50%%/75%% ladder). Valid range 1 to 3.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_extra->SetMinMax(1, 3);

	LSFG_AddKeys(section);
}

// Two-deep history (tightly packed BGRX, top-down rows).
static std::vector<uint32_t> fg_prev;
static std::vector<uint32_t> fg_curr;
static int fg_w = 0;
static int fg_h = 0;

void FRAMEGEN_PushFrame(const uint32_t* pixels, int w_px, int h_px)
{
	if (!pixels || w_px <= 0 || h_px <= 0) {
		return;
	}
	const auto n = static_cast<size_t>(w_px) * static_cast<size_t>(h_px);
	if (w_px != fg_w || h_px != fg_h) {
		// Mode switch: restart the pair, no blend across sizes.
		fg_curr.assign(pixels, pixels + n);
		fg_prev.clear();
		fg_w = w_px;
		fg_h = h_px;
		return;
	}
	fg_prev = fg_curr;
	fg_curr.assign(pixels, pixels + n);
}

bool FRAMEGEN_HavePair(int& w_px_out, int& h_px_out)
{
	if (fg_prev.empty() || fg_curr.empty() ||
	    fg_prev.size() != fg_curr.size()) {
		return false;
	}
	w_px_out = fg_w;
	h_px_out = fg_h;
	return true;
}

bool FRAMEGEN_Blend(float alpha_0_to_1,
                    std::vector<uint32_t>& pixels_out,
                    int& w_px_out, int& h_px_out)
{
	int w = 0, h = 0;
	if (!FRAMEGEN_HavePair(w, h)) {
		return false;
	}
	const float a = std::clamp(alpha_0_to_1, 0.0f, 1.0f);
	const float b = 1.0f - a;
	pixels_out.resize(fg_curr.size());
	for (size_t i = 0; i < fg_curr.size(); ++i) {
		const uint32_t p0 = fg_prev[i];
		const uint32_t p1 = fg_curr[i];
		const auto ch = [&](int shift) -> uint32_t {
			const float v0 = static_cast<float>((p0 >> shift) & 0xff);
			const float v1 = static_cast<float>((p1 >> shift) & 0xff);
			return static_cast<uint32_t>(v0 * b + v1 * a + 0.5f);
		};
		pixels_out[i] = (ch(0)) | (ch(8) << 8) | (ch(16) << 16) |
		                (static_cast<uint32_t>(0xff) << 24);
	}
	w_px_out = w;
	h_px_out = h;
	return true;
}

bool FRAMEGEN_DesktopEnabled()
{
	auto section = get_section("framegen");
	return section && section->GetBool("framegen_desktop");
}

bool FRAMEGEN_XrEnabled()
{
	auto section = get_section("framegen");
	return section && section->GetBool("framegen_xr");
}

int FRAMEGEN_DesktopExtra()
{
	auto section = get_section("framegen");
	if (!section) {
		return 1;
	}
	return std::clamp(section->GetInt("framegen_desktop_extra"), 1, 3);
}

void FRAMEGEN_MaybeEnsureLsfg()
{
	auto section = get_section("framegen");
	if (!section || section->GetString("framegen_method") != "lsfg") {
		return;
	}
	// Only stage shaders when generation is actually wanted somewhere;
	// flipping the method alone does not touch the DLL.
	if (!FRAMEGEN_DesktopEnabled() && !FRAMEGEN_XrEnabled()) {
		return;
	}
	if (LSFG_EnsureReady()) {
		static bool staged_logged = false;
		if (!staged_logged) {
			staged_logged = true;
			LOG_MSG("%s; optical-flow dispatch follows, "
			        "blend active meanwhile",
			        LSFG_StatusText().c_str());
		}
	}
}

bool FRAMEGEN_MethodIsLsfg()
{
	auto section = get_section("framegen");
	return section && section->GetString("framegen_method") == "lsfg";
}

int FRAMEGEN_GenerateLsfg(int gen_count,
                           std::vector<std::vector<uint32_t>>& frames_out,
                           int& w_px_out, int& h_px_out)
{
	int w = 0, h = 0;
	auto section = get_section("framegen");
	const bool have_pair = FRAMEGEN_HavePair(w, h);
	if (!have_pair || gen_count < 1 || gen_count > 7) {
		return 0;
	}
	if (!section || !section->GetBool("framegen_lsfg_gpu")) {
		return 0;
	}
	LSFGVK_SetPreferFp32(section->GetBool("framegen_lsfg_fp32"));
	LSFGVK_SetPerfModel(section->GetBool("framegen_lsfg_perf"));
	LSFGVK_SetFlowScale(section->GetInt("framegen_lsfg_flow_scale"));
	if (!LSFG_EnsureReady()) {
		return 0;
	}
	std::vector<std::vector<uint32_t>> frames;
	const auto t0 = std::chrono::steady_clock::now();
	if (!LSFGVK_Generate(fg_prev.data(), fg_curr.data(), w, h, gen_count,
	                     frames) ||
	    frames.size() != static_cast<size_t>(gen_count)) {
		return 0;
	}
	const auto dt_us = std::chrono::duration_cast<std::chrono::microseconds>(
	        std::chrono::steady_clock::now() - t0);
	// Steady-state GPU cost (upload + dispatch + readback per call).
	// Logged periodically: the number to watch on headset runs.
	static long long us_total = 0;
	static int n_gpu           = 0;
	static int last_w = 0, last_h = 0, last_gen = 0;
	if (w != last_w || h != last_h || gen_count != last_gen) {
		last_w = w;
		last_h = h;
		last_gen = gen_count;
		us_total = 0;
		n_gpu    = 0;
	}
	us_total += dt_us.count();
	if (++n_gpu % 300 == 0) {
		LOG_MSG("LSFG: GPU avg %.2f ms/generate over %d runs (%dx%d x%d)",
		        us_total / 1000.0 / n_gpu, n_gpu, w, h, gen_count);
	}
	frames_out  = std::move(frames);
	w_px_out    = w;
	h_px_out    = h;
	static bool gpu_logged = false;
	if (!gpu_logged) {
		gpu_logged = true;
		LOG_MSG("LSFG: GPU optical-flow dispatch live (%dx%d x%d)%s",
		        w, h, gen_count,
		        LSFGVK_IsUp() ? "" : " (warming up)");
		// One-time coherence check: on a static scene the flow is ~0
		// so the generated frame should closely match the input.
		// Large values here mean wrong bindings/swizzle, not art.
		double acc = 0;
		const auto& f0 = frames_out[0];
		for (size_t i = 0; i < f0.size(); ++i) {
			const uint32_t a = f0[i], b = fg_curr[i];
			acc += std::abs(int(a & 0xff) - int(b & 0xff));
			acc += std::abs(int((a >> 8) & 0xff) - int((b >> 8) & 0xff));
			acc += std::abs(int((a >> 16) & 0xff) - int((b >> 16) & 0xff));
		}
		LOG_MSG("LSFG: First-frame sanity mean abs diff vs input = %.2f/255",
		        acc / f0.size() / 3.0);
	}
	return gen_count;
}
