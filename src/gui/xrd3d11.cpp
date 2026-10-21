// SPDX-FileCopyrightText:  2025-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Dedicated D3D11 device for the OpenXR D3D11 graphics binding plus a
// shader-free compositor. See xrd3d11.h for the contract.

#include "xrd3d11.h"

#include <cstring>
#include <unordered_map>
#include <vector>
#include <dxgi.h>

#include "dosbox.h"

#if C_XR_D3D11

#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Device state.
// ---------------------------------------------------------------------------

static ComPtr<ID3D11Device>        d3d_device;
static ComPtr<ID3D11DeviceContext> d3d_context;
static ComPtr<ID3D11VideoDevice>   d3d_video;
static ComPtr<ID3D11VideoContext> d3d_video_ctx;

// Staging texture (DEFAULT usage so it can feed the video processor;
// UpdateSubresource for CPU uploads). Recreated on size/format change.
static ComPtr<ID3D11Texture2D> d3d_stage_tex;
static int                     d3d_stage_w   = 0;
static int                     d3d_stage_h   = 0;
static DXGI_FORMAT             d3d_stage_fmt = DXGI_FORMAT_UNKNOWN;
static ComPtr<ID3D11VideoProcessorInputView> d3d_stage_view;

// The video processor does not accept sRGB surfaces, so the staging
// texture and the processor output use the UNORM counterpart of the
// swapchain format. The final CopySubresourceRegion into the sRGB
// swapchain image is format-compatible and does no conversion.
static DXGI_FORMAT storage_format(const DXGI_FORMAT fmt)
{
	switch (fmt) {
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
	default: return fmt;
	}
}

// Processor output surface (UNORM, eye-sized, shared by all layers)
// and its full-texture render target for the black letterbox clear.
static ComPtr<ID3D11Texture2D>        d3d_vp_out_tex;
static ComPtr<ID3D11RenderTargetView> d3d_vp_out_rtv;
static int                            d3d_vp_out_w = 0;
static int                            d3d_vp_out_h = 0;
static ComPtr<ID3D11VideoProcessorOutputView> d3d_vp_out_view;
static bool d3d_view_fail_logged = false;
// Retained upload scratch (grows to the largest frame seen).
static std::vector<uint8_t> d3d_upload_tmp;
// Processor permanently unusable (logged once; CPU fallback covers it).
static bool d3d_proc_dead = false;

// Video processor + enumerator, cached per input/output geometry.
static ComPtr<ID3D11VideoProcessorEnumerator> d3d_proc_enum;
static ComPtr<ID3D11VideoProcessor>            d3d_proc;
static int d3d_proc_in_w = 0, d3d_proc_in_h = 0;
static int d3d_proc_out_w = 0, d3d_proc_out_h = 0;
static bool d3d_proc_ok  = false; // rate-conversion caps present

// Per-image render target views (texture handle + array layer).
static std::unordered_map<uint64_t, ComPtr<ID3D11RenderTargetView>> d3d_rtvs;

static DXGI_FORMAT d3d_swap_format = DXGI_FORMAT_UNKNOWN;
static bool        d3d_swap_is_bgra = false;

bool xrd11_is_ready()
{
	return d3d_device && d3d_context;
}

static uint64_t view_key(const uint64_t img, const int layer)
{
	return img ^ (static_cast<uint64_t>(layer) << 56);
}

void xrd11_shutdown()
{
	d3d_rtvs.clear();
	d3d_stage_view.Reset();
	d3d_vp_out_view.Reset();
	d3d_vp_out_rtv.Reset();
	d3d_vp_out_tex.Reset();
	d3d_vp_out_w = d3d_vp_out_h = 0;
	d3d_view_fail_logged       = false;
	d3d_stage_tex.Reset();
	d3d_stage_w = d3d_stage_h = 0;
	d3d_stage_fmt             = DXGI_FORMAT_UNKNOWN;
	d3d_proc.Reset();
	d3d_proc_enum.Reset();
	d3d_proc_in_w = d3d_proc_in_h = d3d_proc_out_w = d3d_proc_out_h = 0;
	d3d_proc_ok                                            = false;
	d3d_proc_dead                                          = false;
	d3d_upload_tmp.clear();
	d3d_upload_tmp.shrink_to_fit();
	d3d_video_ctx.Reset();
	d3d_video.Reset();
	d3d_context.Reset();
	d3d_device.Reset();
	d3d_swap_format  = DXGI_FORMAT_UNKNOWN;
	d3d_swap_is_bgra = false;
}

// Adapter matching the runtime's LUID (nullptr = default adapter).
static IDXGIAdapter1* find_adapter(const LUID& want_luid, bool& exact)
{
	exact = false;
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
		return nullptr;
	}
	ComPtr<IDXGIAdapter1> match;
	for (UINT i = 0;; ++i) {
		ComPtr<IDXGIAdapter1> adapter;
		if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) {
			break;
		}
		DXGI_ADAPTER_DESC1 desc = {};
		if (FAILED(adapter->GetDesc1(&desc))) {
			continue;
		}
		if (!match) {
			match = adapter; // first adapter as fallback
		}
		if (desc.AdapterLuid.HighPart == want_luid.HighPart &&
		    desc.AdapterLuid.LowPart == want_luid.LowPart) {
			match = adapter;
			exact = true;
			break;
		}
	}
	return match.Detach();
}

bool xrd11_create(XrInstance xr_instance, XrSystemId xr_system,
                  XrGraphicsBindingD3D11KHR* binding_out)
{
	if (xrd11_is_ready()) {
		return true;
	}
	if (!binding_out || xr_instance == XR_NULL_HANDLE ||
	    xr_system == XR_NULL_SYSTEM_ID) {
		return false;
	}

	PFN_xrGetD3D11GraphicsRequirementsKHR pfnReqs = nullptr;
	const XrResult res = xrGetInstanceProcAddr(
	        xr_instance,
	        "xrGetD3D11GraphicsRequirementsKHR",
	        reinterpret_cast<PFN_xrVoidFunction*>(&pfnReqs));
	if (res != XR_SUCCESS || !pfnReqs) {
		LOG_ERR("OpenXR: xrGetD3D11GraphicsRequirementsKHR unavailable: %d",
		        res);
		return false;
	}
	XrGraphicsRequirementsD3D11KHR reqs = {};
	reqs.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
	if (pfnReqs(xr_instance, xr_system, &reqs) != XR_SUCCESS) {
		LOG_ERR("OpenXR: xrGetD3D11GraphicsRequirementsKHR failed");
		return false;
	}
	LOG_MSG("OpenXR: D3D11 requirements adapter LUID %08x:%08x, min feature level 0x%x",
	        static_cast<unsigned int>(reqs.adapterLuid.HighPart),
	        static_cast<unsigned int>(reqs.adapterLuid.LowPart),
	        static_cast<unsigned int>(reqs.minFeatureLevel));

	bool exact = false;
	ComPtr<IDXGIAdapter1> adapter;
	adapter.Attach(find_adapter(reqs.adapterLuid, exact));
	if (!exact) {
		LOG_WARNING("OpenXR: Runtime D3D11 adapter not found, using default");
	}

	static constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
	                                               D3D_FEATURE_LEVEL_11_0};
	D3D_FEATURE_LEVEL got_level = D3D_FEATURE_LEVEL_9_1;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	HRESULT hr = D3D11CreateDevice(adapter.Get(),
	                               adapter ? D3D_DRIVER_TYPE_UNKNOWN
	                                       : D3D_DRIVER_TYPE_HARDWARE,
	                               nullptr,
	                               0,
	                               levels,
	                               2,
	                               D3D11_SDK_VERSION,
	                               &device,
	                               &got_level,
	                               &context);
	if (FAILED(hr)) {
		LOG_ERR("OpenXR: D3D11CreateDevice failed: 0x%08x",
		        static_cast<unsigned int>(hr));
		return false;
	}
	if (got_level < reqs.minFeatureLevel) {
		LOG_ERR("OpenXR: D3D11 feature level 0x%x below runtime minimum 0x%x",
		        static_cast<unsigned int>(got_level),
		        static_cast<unsigned int>(reqs.minFeatureLevel));
		return false;
	}
	d3d_device  = device;
	d3d_context = context;

	// Video processor is optional: without it the fitted path falls
	// back to CPU scaling (still correct, just slower). Its methods
	// live on ID3D11VideoContext (QI'd from the immediate context).
	if (SUCCEEDED(d3d_device.As(&d3d_video)) &&
	    SUCCEEDED(d3d_context.As(&d3d_video_ctx))) {
		LOG_MSG("OpenXR: D3D11 video processor available");
	} else {
		d3d_video.Reset();
		d3d_video_ctx.Reset();
		LOG_WARNING("OpenXR: No D3D11 video device; eye scaling falls back to CPU");
	}

	binding_out->type   = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR;
	binding_out->next   = nullptr;
	binding_out->device = d3d_device.Get();
	LOG_MSG("OpenXR: D3D11 binding ready (feature level 0x%x%s)",
	        static_cast<unsigned int>(got_level),
	        exact ? "" : ", default adapter");
	return true;
}

int64_t xrd11_pick_format(const int64_t* formats, const uint32_t count)
{
	d3d_swap_format  = DXGI_FORMAT_UNKNOWN;
	d3d_swap_is_bgra = false;
	if (!formats || count == 0) {
		return 0;
	}
	// hello_xr order: sRGB RGBA8 first (matches the GL sRGB8 pick).
	static constexpr int64_t prefs[] = {
	        static_cast<int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB),
	        static_cast<int64_t>(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB),
	        static_cast<int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM),
	        static_cast<int64_t>(DXGI_FORMAT_B8G8R8A8_UNORM)};
	int64_t picked = formats[0];
	for (const auto want : prefs) {
		for (uint32_t i = 0; i < count; ++i) {
			if (formats[i] == want) {
				picked = want;
				break;
			}
		}
		if (picked == want) {
			break;
		}
	}
	d3d_swap_format = static_cast<DXGI_FORMAT>(picked);
	d3d_swap_is_bgra =
	        (d3d_swap_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
	         d3d_swap_format == DXGI_FORMAT_B8G8R8A8_UNORM);
	LOG_MSG("OpenXR: D3D11 swapchain format %lld%s",
	        static_cast<long long>(picked),
	        d3d_swap_is_bgra ? " (BGRA, R/B swapped on upload)" : "");
	return picked;
}

bool xrd11_enumerate(XrSwapchain swapchain, const char* label,
                     std::vector<XrGfxImage>& out)
{
	out.clear();
	if (!xrd11_is_ready() || swapchain == XR_NULL_HANDLE) {
		return false;
	}
	uint32_t image_count = 0;
	if (xrEnumerateSwapchainImages(swapchain, 0, &image_count, nullptr) !=
	            XR_SUCCESS ||
	    image_count == 0) {
		LOG_ERR("OpenXR: Failed to enumerate %s swapchain images", label);
		return false;
	}
	std::vector<XrSwapchainImageD3D11KHR> tmp(image_count);
	for (auto& img : tmp) {
		img.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
		img.next = nullptr;
	}
	if (xrEnumerateSwapchainImages(
	            swapchain,
	            image_count,
	            &image_count,
	            reinterpret_cast<XrSwapchainImageBaseHeader*>(
	                    tmp.data())) != XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to enumerate %s swapchain images", label);
		return false;
	}
	out.resize(image_count);
	for (uint32_t i = 0; i < image_count; ++i) {
		out[i].type   = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
		out[i].handle = reinterpret_cast<uint64_t>(tmp[i].texture);
	}
	LOG_MSG("OpenXR: %s swapchain ready (%u D3D11 textures)", label,
	        image_count);
	return true;
}

// ---------------------------------------------------------------------------
// Staging + upload.
// ---------------------------------------------------------------------------

// (Re)create the staging texture in the storage (UNORM) format
// (DEFAULT usage + render-target bind so it can feed the video
// processor; UpdateSubresource for CPU uploads).
static bool ensure_staging(const int w_px, const int h_px)
{
	if (w_px == d3d_stage_w && h_px == d3d_stage_h && d3d_stage_tex &&
	    d3d_stage_fmt == d3d_swap_format) {
		return true;
	}
	// A new staging texture invalidates the processor input view.
	d3d_stage_view.Reset();
	d3d_stage_tex.Reset();
	d3d_stage_w = d3d_stage_h = 0;
	d3d_stage_fmt             = DXGI_FORMAT_UNKNOWN;
	if (w_px <= 0 || h_px <= 0 ||
	    d3d_swap_format == DXGI_FORMAT_UNKNOWN) {
		return false;
	}
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width                = static_cast<UINT>(w_px);
	desc.Height               = static_cast<UINT>(h_px);
	desc.MipLevels            = 1;
	desc.ArraySize            = 1;
	desc.Format               = storage_format(d3d_swap_format);
	desc.SampleDesc.Count     = 1;
	desc.Usage                = D3D11_USAGE_DEFAULT;
	desc.BindFlags            = D3D11_BIND_RENDER_TARGET |
	                            D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(d3d_device->CreateTexture2D(&desc, nullptr,
	                                       &d3d_stage_tex))) {
		LOG_ERR("OpenXR: D3D11 staging texture create failed");
		return false;
	}
	d3d_stage_w   = w_px;
	d3d_stage_h   = h_px;
	d3d_stage_fmt = d3d_swap_format;
	return true;
}

// Copy/convert bottom-up source rows into a top-down staging upload
// (swap R/B when source and swapchain byte orders differ).
static void fill_staging(uint8_t* dst, const size_t dst_pitch,
                         const uint32_t* src_bottom_up, const int w_px,
                         const int h_px, const bool src_is_bgra,
                         const bool dst_is_bgra)
{
	const bool swap = (src_is_bgra != dst_is_bgra);
	for (int y = 0; y < h_px; ++y) {
		const uint32_t* srow =
		        src_bottom_up +
		        static_cast<size_t>(h_px - 1 - y) * w_px;
		uint8_t* drow = dst + static_cast<size_t>(y) * dst_pitch;
		if (!swap) {
			std::memcpy(drow, srow,
			            static_cast<size_t>(w_px) * sizeof(uint32_t));
		} else {
			for (int x = 0; x < w_px; ++x) {
				const uint32_t p = srow[x];
				drow[4 * x + 0]   = static_cast<uint8_t>(p >> 16);
				drow[4 * x + 1]   = static_cast<uint8_t>(p >> 8);
				drow[4 * x + 2]   = static_cast<uint8_t>(p);
				drow[4 * x + 3]   = static_cast<uint8_t>(p >> 24);
			}
		}
	}
}

// Upload through UpdateSubresource (works on DEFAULT textures; pitch
// in bytes, must cover w*4).
static void update_staging(const uint32_t* src_bottom_up, const int w_px,
                           const int h_px, const bool src_is_bgra)
{
	const size_t row_bytes = static_cast<size_t>(w_px) * sizeof(uint32_t);
	if (d3d_upload_tmp.size() < row_bytes * h_px) {
		d3d_upload_tmp.resize(row_bytes * h_px);
	}
	fill_staging(d3d_upload_tmp.data(), row_bytes, src_bottom_up, w_px,
	             h_px, src_is_bgra, d3d_swap_is_bgra);
	D3D11_BOX box = {};
	box.left = box.top = box.front = 0;
	box.right  = static_cast<UINT>(w_px);
	box.bottom = static_cast<UINT>(h_px);
	box.back   = 1;
	d3d_context->UpdateSubresource(d3d_stage_tex.Get(), 0, &box,
	                               d3d_upload_tmp.data(),
	                               static_cast<UINT>(row_bytes), 0);
}

// Bilinear upscale of bottom-up BGRA source into a top-down dst
// buffer in dst byte order. Fallback when the video processor is
// unavailable.
static void bilinear_scale(std::vector<uint32_t>& dst, const int dw_px,
                           const int dh_px, const uint32_t* src_bottom_up,
                           const int sw_px, const int sh_px,
                           const bool dst_is_bgra)
{
	if (sw_px <= 0 || sh_px <= 0 || dw_px <= 0 || dh_px <= 0) {
		return;
	}
	dst.assign(static_cast<size_t>(dw_px) * dh_px, 0xFF000000u);
	const float sx = static_cast<float>(sw_px) / dw_px;
	const float sy = static_cast<float>(sh_px) / dh_px;
	for (int y = 0; y < dh_px; ++y) {
		const float v  = (y + 0.5f) * sy - 0.5f;
		const float vf = (sh_px - 1.0f) - v;
		int y0         = static_cast<int>(vf);
		float fy       = vf - y0;
		if (y0 < 0) {
			y0 = 0;
			fy = 0.0f;
		}
		if (y0 >= sh_px - 1) {
			y0 = sh_px - 1;
			fy = 0.0f;
		}
		const int y1 = (y0 + 1 < sh_px) ? y0 + 1 : y0;
		for (int x = 0; x < dw_px; ++x) {
			const float u = (x + 0.5f) * sx - 0.5f;
			int x0        = static_cast<int>(u);
			float fx      = u - x0;
			if (x0 < 0) {
				x0 = 0;
				fx = 0.0f;
			}
			if (x0 >= sw_px - 1) {
				x0 = sw_px - 1;
				fx = 0.0f;
			}
			const int x1 = (x0 + 1 < sw_px) ? x0 + 1 : x0;
			const uint32_t p00 = src_bottom_up[y0 * sw_px + x0];
			const uint32_t p10 = src_bottom_up[y0 * sw_px + x1];
			const uint32_t p01 = src_bottom_up[y1 * sw_px + x0];
			const uint32_t p11 = src_bottom_up[y1 * sw_px + x1];
			const float w00 = (1 - fx) * (1 - fy);
			const float w10 = fx * (1 - fy);
			const float w01 = (1 - fx) * fy;
			const float w11 = fx * fy;
			const float b = ((p00) & 0xFFu) * w00 +
			                ((p10) & 0xFFu) * w10 +
			                ((p01) & 0xFFu) * w01 +
			                ((p11) & 0xFFu) * w11;
			const float g = ((p00 >> 8) & 0xFFu) * w00 +
			                ((p10 >> 8) & 0xFFu) * w10 +
			                ((p01 >> 8) & 0xFFu) * w01 +
			                ((p11 >> 8) & 0xFFu) * w11;
			const float r = ((p00 >> 16) & 0xFFu) * w00 +
			                ((p10 >> 16) & 0xFFu) * w10 +
			                ((p01 >> 16) & 0xFFu) * w01 +
			                ((p11 >> 16) & 0xFFu) * w11;
			const auto R = static_cast<uint32_t>(r + 0.5f);
			const auto G = static_cast<uint32_t>(g + 0.5f);
			const auto B = static_cast<uint32_t>(b + 0.5f);
			dst[static_cast<size_t>(y) * dw_px + x] =
			        dst_is_bgra ? (0xFF000000u | (R << 16) | (G << 8) | B)
			                    : (0xFF000000u | (B << 16) | (G << 8) | R);
		}
	}
}

static ID3D11Texture2D* to_tex(const uint64_t handle)
{
	return reinterpret_cast<ID3D11Texture2D*>(
	        static_cast<uintptr_t>(handle));
}

static ID3D11RenderTargetView* get_rtv(const uint64_t handle,
                                       const int layer)
{
	const auto key = view_key(handle, layer);
	const auto it  = d3d_rtvs.find(key);
	if (it != d3d_rtvs.end()) {
		return it->second.Get();
	}
	D3D11_RENDER_TARGET_VIEW_DESC desc = {};
	desc.Format                        = d3d_swap_format;
	desc.ViewDimension                  = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
	desc.Texture2DArray.MipSlice        = 0;
	desc.Texture2DArray.FirstArraySlice = static_cast<UINT>(layer);
	desc.Texture2DArray.ArraySize       = 1;
	ComPtr<ID3D11RenderTargetView> rtv;
	if (FAILED(d3d_device->CreateRenderTargetView(to_tex(handle), &desc,
	                                             &rtv))) {
		return nullptr;
	}
	d3d_rtvs[key] = rtv;
	return rtv.Get();
}

// (Re)create the video processor + input view for a geometry change.
// False when the processor is unavailable (CPU fallback covers it).
static bool ensure_processor(const int in_w_px, const int in_h_px,
                             const int out_w_px, const int out_h_px)
{
	if (!d3d_video || d3d_proc_dead) {
		return false;
	}
	if (d3d_proc && in_w_px == d3d_proc_in_w &&
	    in_h_px == d3d_proc_in_h && out_w_px == d3d_proc_out_w &&
	    out_h_px == d3d_proc_out_h) {
		return d3d_proc_ok;
	}
	d3d_proc.Reset();
	d3d_proc_enum.Reset();
	d3d_proc_ok = false;
	D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
	content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	// Any failure below retires the processor permanently for this
	// session (logged once here; silent afterwards).
	d3d_proc_dead = true;
	content.InputFrameRate.Numerator   = 60;
	content.InputFrameRate.Numerator   = 60;
	content.InputFrameRate.Denominator = 1;
	content.InputWidth  = static_cast<UINT>(in_w_px);
	content.InputHeight = static_cast<UINT>(in_h_px);
	content.OutputFrameRate.Numerator   = 60;
	content.OutputFrameRate.Denominator = 1;
	content.OutputWidth  = static_cast<UINT>(out_w_px);
	content.OutputHeight = static_cast<UINT>(out_h_px);
	content.Usage        = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
	if (FAILED(d3d_video->CreateVideoProcessorEnumerator(&content,
	                                                     &d3d_proc_enum))) {
		LOG_WARNING("OpenXR: D3D11 video processor enumerator failed");
		return false;
	}
	D3D11_VIDEO_PROCESSOR_CAPS caps = {};
	if (FAILED(d3d_proc_enum->GetVideoProcessorCaps(&caps)) ||
	    caps.RateConversionCapsCount == 0) {
		LOG_WARNING("OpenXR: D3D11 video processor has no rate caps");
		return false;
	}
	UINT fmt_flags = 0;
	const auto op_fmt = storage_format(d3d_swap_format);
	if (FAILED(d3d_proc_enum->CheckVideoProcessorFormat(op_fmt,
	                                                    &fmt_flags)) ||
	    !(fmt_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
	    !(fmt_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
		LOG_WARNING("OpenXR: D3D11 video processor lacks RGBA support");
		return false;
	}
	if (FAILED(d3d_video->CreateVideoProcessor(d3d_proc_enum.Get(), 0,
	                                           &d3d_proc))) {
		LOG_WARNING("OpenXR: D3D11 video processor create failed");
		return false;
	}
	// Pixel-fidelity: no driver "enhancements", progressive frames.
	d3d_video_ctx->VideoProcessorSetStreamAutoProcessingMode(d3d_proc.Get(),
	                                                       0,
	                                                       FALSE);
	d3d_video_ctx->VideoProcessorSetStreamFrameFormat(d3d_proc.Get(), 0,
	                                                D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
	d3d_proc_in_w = in_w_px;
	d3d_proc_in_h = in_h_px;
	d3d_proc_out_w = out_w_px;
	d3d_proc_out_h = out_h_px;
	d3d_proc_ok   = true;
	d3d_proc_dead = false;
	// The views belong to the old enumerator: drop them so the next
	// upload rebuilds them against the new processor.
	d3d_stage_view.Reset();
	d3d_vp_out_view.Reset();
	d3d_view_fail_logged = false;
	return true;
}

static ID3D11VideoProcessorInputView* get_input_view()
{
	if (!d3d_proc || !d3d_stage_tex) {
		return nullptr;
	}
	if (!d3d_stage_view) {
		D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc = {};
		desc.FourCC         = 0;
		desc.ViewDimension   = D3D11_VPIV_DIMENSION_TEXTURE2D;
		desc.Texture2D.MipSlice   = 0;
		desc.Texture2D.ArraySlice = 0;
		if (FAILED(d3d_video->CreateVideoProcessorInputView(
		            d3d_stage_tex.Get(), d3d_proc_enum.Get(), &desc,
		            &d3d_stage_view))) {
			if (!d3d_view_fail_logged) {
				LOG_WARNING("OpenXR: D3D11 processor input view failed");
				d3d_view_fail_logged = true;
			}
			return nullptr;
		}
	}
	return d3d_stage_view.Get();
}

// (Re)create the eye-sized processor output surface and its views.
static bool ensure_vp_output(const int w_px, const int h_px)
{
	if (d3d_vp_out_tex && w_px == d3d_vp_out_w &&
	    h_px == d3d_vp_out_h && d3d_vp_out_view && d3d_vp_out_rtv) {
		return true;
	}
	d3d_vp_out_view.Reset();
	d3d_vp_out_rtv.Reset();
	d3d_vp_out_tex.Reset();
	d3d_vp_out_w = d3d_vp_out_h = 0;
	if (w_px <= 0 || h_px <= 0 ||
	    d3d_swap_format == DXGI_FORMAT_UNKNOWN) {
		return false;
	}
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width                = static_cast<UINT>(w_px);
	desc.Height               = static_cast<UINT>(h_px);
	desc.MipLevels            = 1;
	desc.ArraySize            = 1;
	desc.Format               = storage_format(d3d_swap_format);
	desc.SampleDesc.Count     = 1;
	desc.Usage                = D3D11_USAGE_DEFAULT;
	desc.BindFlags            = D3D11_BIND_RENDER_TARGET |
	                            D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(d3d_device->CreateTexture2D(&desc, nullptr,
	                                       &d3d_vp_out_tex))) {
		LOG_WARNING("OpenXR: D3D11 processor output create failed");
		return false;
	}
	if (FAILED(d3d_device->CreateRenderTargetView(d3d_vp_out_tex.Get(),
	                                              nullptr,
	                                              &d3d_vp_out_rtv))) {
		LOG_WARNING("OpenXR: D3D11 processor output RTV failed");
		d3d_vp_out_tex.Reset();
		return false;
	}
	D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov = {};
	ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
	if (FAILED(d3d_video->CreateVideoProcessorOutputView(
	            d3d_vp_out_tex.Get(), d3d_proc_enum.Get(), &ov,
	            &d3d_vp_out_view))) {
		LOG_WARNING("OpenXR: D3D11 processor output view failed");
		d3d_vp_out_rtv.Reset();
		d3d_vp_out_tex.Reset();
		return false;
	}
	d3d_vp_out_w = w_px;
	d3d_vp_out_h = h_px;
	return true;
}

static ID3D11VideoProcessorOutputView* get_output_view()
{
	return d3d_vp_out_view.Get();
}

static bool upload_full_tex(ID3D11Texture2D* dst_tex, const int layer,
                            const int w_px, const int h_px,
                            const uint32_t* rgba_bottom_up)
{
	if (!ensure_staging(w_px, h_px)) {
		return false;
	}
	// Rasters are RGBA-ordered; swap only for BGRA swapchains.
	update_staging(rgba_bottom_up, w_px, h_px, false);
	d3d_context->CopySubresourceRegion(dst_tex,
	                                  D3D11CalcSubresource(0,
	                                                         layer,
	                                                         1),
	                                  0, 0, 0, d3d_stage_tex.Get(), 0,
	                                  nullptr);
	d3d_context->Flush();
	return true;
}

bool xrd11_upload_full(const uint64_t dst_handle, const int dst_layer,
                       const int w_px, const int h_px,
                       const uint32_t* rgba_bottom_up)
{
	if (!xrd11_is_ready() || !dst_handle || !rgba_bottom_up ||
	    w_px <= 0 || h_px <= 0 ||
	    d3d_swap_format == DXGI_FORMAT_UNKNOWN) {
		return false;
	}
	return upload_full_tex(to_tex(dst_handle), dst_layer, w_px, h_px,
	                       rgba_bottom_up);
}

bool xrd11_upload_fitted(const uint64_t dst_handle, const int dst_layer,
                         const int eye_w_px, const int eye_h_px,
                         const int dx_px, const int dy_px_top,
                         const int dw_px, const int dh_px,
                         const uint32_t* bgra_bottom_up,
                         const int src_w_px, const int src_h_px)
{
	if (!xrd11_is_ready() || !dst_handle || !bgra_bottom_up ||
	    eye_w_px <= 0 || eye_h_px <= 0 || src_w_px <= 0 ||
	    src_h_px <= 0 || d3d_swap_format == DXGI_FORMAT_UNKNOWN) {
		return false;
	}
	ID3D11Texture2D* dst_tex = to_tex(dst_handle);

	// Clip the blit rect to the image (the disparity shift can push it
	// past the edge); the source is full-frame either way.
	int ddx0 = dx_px, ddy0 = dy_px_top;
	int ddx1 = dx_px + dw_px, ddy1 = dy_px_top + dh_px;
	if (ddx0 < 0) {
		ddx0 = 0;
	}
	if (ddy0 < 0) {
		ddy0 = 0;
	}
	if (ddx1 > eye_w_px) {
		ddx1 = eye_w_px;
	}
	if (ddy1 > eye_h_px) {
		ddy1 = eye_h_px;
	}
	const bool do_blit = (ddx1 > ddx0 && ddy1 > ddy0 && dw_px > 0 &&
	                      dh_px > 0);

	// Letterbox: clear first (the blit only covers the picture rect).
	ID3D11RenderTargetView* rtv = get_rtv(dst_handle, dst_layer);
	if (!rtv) {
		return false;
	}
	static constexpr float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	d3d_context->ClearRenderTargetView(rtv, black);

	if (!do_blit) {
		d3d_context->Flush();
		return true; // cleared image stands
	}
	if (ensure_staging(src_w_px, src_h_px) &&
	    ensure_processor(src_w_px, src_h_px, eye_w_px, eye_h_px) &&
	    ensure_vp_output(eye_w_px, eye_h_px)) {
		// Staging and processor output are UNORM (like the
		// swapchain); convert the BGRA game bytes when the byte
		// order differs.
		update_staging(bgra_bottom_up, src_w_px, src_h_px, true);
		ID3D11VideoProcessorInputView* in_view = get_input_view();
		ID3D11VideoProcessorOutputView* out_view = get_output_view();
		if (in_view && out_view) {
			d3d_context->ClearRenderTargetView(d3d_vp_out_rtv.Get(),
			                                   black);
			D3D11_VIDEO_PROCESSOR_STREAM stream = {};
			stream.Enable           = TRUE;
			stream.pInputSurface    = in_view;
			D3D11_RECT src_box = {0, 0, src_w_px, src_h_px};
			D3D11_RECT dst_box = {ddx0, ddy0, ddx1, ddy1};
			D3D11_RECT tgt_box = {0, 0, eye_w_px, eye_h_px};
			d3d_video_ctx->VideoProcessorSetStreamSourceRect(
			        d3d_proc.Get(), 0, TRUE, &src_box);
			d3d_video_ctx->VideoProcessorSetStreamDestRect(
			        d3d_proc.Get(), 0, TRUE, &dst_box);
			d3d_video_ctx->VideoProcessorSetOutputTargetRect(
			        d3d_proc.Get(), TRUE, &tgt_box);
			if (SUCCEEDED(d3d_video_ctx->VideoProcessorBlt(
			                d3d_proc.Get(), out_view, 0, 1,
			                &stream))) {
				d3d_context->CopySubresourceRegion(
				        dst_tex,
				        D3D11CalcSubresource(0,
				                             static_cast<UINT>(
				                                     dst_layer),
				                             1),
				        0, 0, 0, d3d_vp_out_tex.Get(), 0, nullptr);
				d3d_context->Flush();
				return true;
			}
			LOG_WARNING("OpenXR: D3D11 VideoProcessorBlt failed");
		}
	}

	// No-blit fallback: CPU-scale to a full-eye buffer and copy it.
	std::vector<uint32_t> scaled;
	bilinear_scale(scaled, eye_w_px, eye_h_px, bgra_bottom_up, src_w_px,
	               src_h_px, d3d_swap_is_bgra);
	if (scaled.empty()) {
		d3d_context->Flush();
		return true; // cleared image stands
	}
	// Top-down scaled buffer -> bottom-up for the shared uploader.
	std::vector<uint32_t> flipped(
	        static_cast<size_t>(eye_w_px) * eye_h_px);
	for (int y = 0; y < eye_h_px; ++y) {
		std::memcpy(flipped.data() +
		                    static_cast<size_t>(eye_h_px - 1 - y) *
		                            eye_w_px,
		            scaled.data() + static_cast<size_t>(y) * eye_w_px,
		            static_cast<size_t>(eye_w_px) *
		                    sizeof(uint32_t));
	}
	return upload_full_tex(dst_tex, dst_layer, eye_w_px, eye_h_px,
	                       flipped.data());
}

#endif // C_XR_D3D11
