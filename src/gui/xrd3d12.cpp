// SPDX-FileCopyrightText:  2025-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Dedicated D3D12 device for the OpenXR D3D12 graphics binding plus a
// textured-quad compositor. See xrd3d12.h for the contract.

#include "xrd3d12.h"

#include <cstring>
#include <unordered_map>
#include <vector>
#include <dxgi.h>
#include <d3dcompiler.h>

#include "dosbox.h"

#if C_XR_D3D12

#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Trivial textured fullscreen triangle (SV_VertexID, no vertex
// buffer). UV (0,0) is the texture top-left; the viewport maps NDC
// into the destination rect, so the full source lands fitted.
static const char* d3d12_quad_hlsl = R"(
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct PSIn {
	float4 pos : SV_POSITION;
	float2 uv : TEXCOORD0;
};
PSIn MainVS(uint vid : SV_VertexID)
{
	float2 p = float2((vid << 1) & 2, vid & 2);
	PSIn o;
	o.pos = float4(p * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
	o.uv = p;
	return o;
}
float4 MainPS(PSIn i) : SV_TARGET
{
	return tex.Sample(smp, i.uv);
}
)";

// ---------------------------------------------------------------------------
// Device state.
// ---------------------------------------------------------------------------

static ComPtr<ID3D12Device>       d3d_device;
static ComPtr<ID3D12CommandQueue> d3d_queue;
static ComPtr<ID3D12CommandAllocator> d3d_alloc;
static ComPtr<ID3D12GraphicsCommandList> d3d_list;
static ComPtr<ID3D12Fence>        d3d_fence;
static HANDLE                     d3d_fence_event = nullptr;
static uint64_t                   d3d_fence_value = 0;

// Upload-heap staging texture (CPU-mapped every upload). Recreated on
// size/format change; SRV refreshed with it.
static ComPtr<ID3D12Resource> d3d_stage_tex;
static int                    d3d_stage_w   = 0;
static int                    d3d_stage_h   = 0;
static DXGI_FORMAT            d3d_stage_fmt = DXGI_FORMAT_UNKNOWN;

// Shader-visible heaps: one SRV slot (staging) + one sampler.
static ComPtr<ID3D12DescriptorHeap> d3d_srv_heap;
static ComPtr<ID3D12DescriptorHeap> d3d_samp_heap;

// CPU-only RTV heap; descriptors allocated per (image, layer).
static ComPtr<ID3D12DescriptorHeap> d3d_rtv_heap;
static UINT                         d3d_rtv_used = 0;
static std::unordered_map<uint64_t, D3D12_CPU_DESCRIPTOR_HANDLE> d3d_rtvs;

static ComPtr<ID3D12RootSignature> d3d_root_sig;
static ComPtr<ID3D12PipelineState> d3d_pso_opaque;
static ComPtr<ID3D12PipelineState> d3d_pso_blend;

static DXGI_FORMAT d3d_swap_format = DXGI_FORMAT_UNKNOWN;
static bool        d3d_swap_is_bgra = false;

static uint64_t rtv_key(const uint64_t img, const int layer)
{
	return img ^ (static_cast<uint64_t>(layer) << 56);
}

bool xrd12_is_ready()
{
	return d3d_device && d3d_queue;
}

void xrd12_shutdown()
{
	d3d_rtvs.clear();
	d3d_root_sig.Reset();
	d3d_pso_opaque.Reset();
	d3d_pso_blend.Reset();
	d3d_srv_heap.Reset();
	d3d_samp_heap.Reset();
	d3d_rtv_heap.Reset();
	d3d_rtv_used = 0;
	d3d_stage_tex.Reset();
	d3d_stage_w = d3d_stage_h = 0;
	d3d_stage_fmt             = DXGI_FORMAT_UNKNOWN;
	d3d_list.Reset();
	d3d_alloc.Reset();
	if (d3d_fence_event) {
		CloseHandle(d3d_fence_event);
		d3d_fence_event = nullptr;
	}
	d3d_fence.Reset();
	d3d_fence_value = 0;
	d3d_queue.Reset();
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

static bool compile_shaders()
{
	ComPtr<ID3DBlob> vs, ps, errors;
	HRESULT hr = D3DCompile(d3d12_quad_hlsl,
	                        std::strlen(d3d12_quad_hlsl),
	                        nullptr, nullptr, nullptr, "MainVS",
	                        "vs_5_0", 0, 0, &vs, &errors);
	if (FAILED(hr)) {
		LOG_ERR("OpenXR: D3D12 vertex shader compile failed: %s",
		        errors ? static_cast<const char*>(
		                         errors->GetBufferPointer())
		               : "unknown");
		return false;
	}
	hr = D3DCompile(d3d12_quad_hlsl,
	                std::strlen(d3d12_quad_hlsl),
	                nullptr, nullptr, nullptr, "MainPS",
	                "ps_5_0", 0, 0, &ps, &errors);
	if (FAILED(hr)) {
		LOG_ERR("OpenXR: D3D12 pixel shader compile failed: %s",
		        errors ? static_cast<const char*>(
		                         errors->GetBufferPointer())
		               : "unknown");
		return false;
	}

	// Root signature: SRV table (t0) + sampler table (s0), pixel only.
	D3D12_DESCRIPTOR_RANGE ranges[2] = {};
	ranges[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors     = 1;
	ranges[0].BaseShaderRegister = 0;
	ranges[0].RegisterSpace      = 0;
	ranges[0].OffsetInDescriptorsFromTableStart =
	        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	ranges[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	ranges[1].NumDescriptors     = 1;
	ranges[1].BaseShaderRegister = 0;
	ranges[1].RegisterSpace      = 0;
	ranges[1].OffsetInDescriptorsFromTableStart =
	        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	D3D12_ROOT_PARAMETER params[2] = {};
	params[0].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[0].DescriptorTable.NumDescriptorRanges = 1;
	params[0].DescriptorTable.pDescriptorRanges   = &ranges[0];
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[1].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 1;
	params[1].DescriptorTable.pDescriptorRanges   = &ranges[1];
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC rs_desc = {};
	rs_desc.NumParameters     = 2;
	rs_desc.pParameters       = params;
	rs_desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;
	ComPtr<ID3DBlob> rs_blob;
	if (FAILED(D3D12SerializeRootSignature(&rs_desc,
	                                       D3D_ROOT_SIGNATURE_VERSION_1,
	                                       &rs_blob, &errors))) {
		LOG_ERR("OpenXR: D3D12 root signature serialize failed");
		return false;
	}
	if (FAILED(d3d_device->CreateRootSignature(
	            0, rs_blob->GetBufferPointer(),
	            rs_blob->GetBufferSize(),
	            IID_PPV_ARGS(&d3d_root_sig)))) {
		LOG_ERR("OpenXR: D3D12 root signature create failed");
		return false;
	}

	// One PSO per blend mode (opaque projection, alpha overlays).
	for (int blend = 0; blend < 2; ++blend) {
		D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
		pso.pRootSignature = d3d_root_sig.Get();
		pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
		pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
		pso.BlendState.AlphaToCoverageEnable = FALSE;
		pso.BlendState.IndependentBlendEnable = FALSE;
		auto& rt = pso.BlendState.RenderTarget[0];
		rt.BlendEnable           = blend ? TRUE : FALSE;
		rt.SrcBlend              = D3D12_BLEND_SRC_ALPHA;
		rt.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
		rt.BlendOp               = D3D12_BLEND_OP_ADD;
		rt.SrcBlendAlpha         = D3D12_BLEND_ONE;
		rt.DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
		rt.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
		rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		pso.SampleMask = 0xFFFFFFFFu;
		pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		pso.RasterizerState.CullMode  = D3D12_CULL_MODE_NONE;
		pso.RasterizerState.DepthClipEnable = TRUE;
		pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pso.NumRenderTargets      = 1;
		pso.RTVFormats[0]         = d3d_swap_format;
		pso.SampleDesc.Count      = 1;
		ComPtr<ID3D12PipelineState>& slot =
		        blend ? d3d_pso_blend : d3d_pso_opaque;
		if (FAILED(d3d_device->CreateGraphicsPipelineState(
		            &pso, IID_PPV_ARGS(&slot)))) {
			LOG_ERR("OpenXR: D3D12 pipeline create failed");
			return false;
		}
	}
	return true;
}

bool xrd12_create(XrInstance xr_instance, XrSystemId xr_system,
                  XrGraphicsBindingD3D12KHR* binding_out)
{
	if (xrd12_is_ready()) {
		return true;
	}
	if (!binding_out || xr_instance == XR_NULL_HANDLE ||
	    xr_system == XR_NULL_SYSTEM_ID) {
		return false;
	}

	PFN_xrGetD3D12GraphicsRequirementsKHR pfnReqs = nullptr;
	const XrResult res = xrGetInstanceProcAddr(
	        xr_instance,
	        "xrGetD3D12GraphicsRequirementsKHR",
	        reinterpret_cast<PFN_xrVoidFunction*>(&pfnReqs));
	if (res != XR_SUCCESS || !pfnReqs) {
		LOG_ERR("OpenXR: xrGetD3D12GraphicsRequirementsKHR unavailable: %d",
		        res);
		return false;
	}
	XrGraphicsRequirementsD3D12KHR reqs = {};
	reqs.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR;
	if (pfnReqs(xr_instance, xr_system, &reqs) != XR_SUCCESS) {
		LOG_ERR("OpenXR: xrGetD3D12GraphicsRequirementsKHR failed");
		return false;
	}
	LOG_MSG("OpenXR: D3D12 requirements adapter LUID %08x:%08x, min feature level 0x%x",
	        static_cast<unsigned int>(reqs.adapterLuid.HighPart),
	        static_cast<unsigned int>(reqs.adapterLuid.LowPart),
	        static_cast<unsigned int>(reqs.minFeatureLevel));

	bool exact = false;
	ComPtr<IDXGIAdapter1> adapter;
	adapter.Attach(find_adapter(reqs.adapterLuid, exact));
	if (!adapter) {
		LOG_ERR("OpenXR: No DXGI adapter found");
		return false;
	}
	if (!exact) {
		LOG_WARNING("OpenXR: Runtime D3D12 adapter not found, using default");
	}
	if (FAILED(D3D12CreateDevice(adapter.Get(), reqs.minFeatureLevel,
	                             IID_PPV_ARGS(&d3d_device)))) {
		LOG_ERR("OpenXR: D3D12CreateDevice failed");
		return false;
	}
	D3D12_COMMAND_QUEUE_DESC qdesc = {};
	qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	if (FAILED(d3d_device->CreateCommandQueue(&qdesc,
	                                          IID_PPV_ARGS(&d3d_queue)))) {
		LOG_ERR("OpenXR: D3D12 command queue create failed");
		xrd12_shutdown();
		return false;
	}
	if (FAILED(d3d_device->CreateCommandAllocator(
	            D3D12_COMMAND_LIST_TYPE_DIRECT,
	            IID_PPV_ARGS(&d3d_alloc)))) {
		LOG_ERR("OpenXR: D3D12 command allocator create failed");
		xrd12_shutdown();
		return false;
	}
	if (FAILED(d3d_device->CreateCommandList(
	            0, D3D12_COMMAND_LIST_TYPE_DIRECT, d3d_alloc.Get(),
	            nullptr, IID_PPV_ARGS(&d3d_list)))) {
		LOG_ERR("OpenXR: D3D12 command list create failed");
		xrd12_shutdown();
		return false;
	}
	// Lists start open; close immediately so each upload can reset.
	d3d_list->Close();
	if (FAILED(d3d_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
	                                   IID_PPV_ARGS(&d3d_fence)))) {
		LOG_ERR("OpenXR: D3D12 fence create failed");
		xrd12_shutdown();
		return false;
	}
	d3d_fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (!d3d_fence_event) {
		LOG_ERR("OpenXR: D3D12 fence event create failed");
		xrd12_shutdown();
		return false;
	}
	d3d_fence_value = 0;

	// SRV heap (one staging slot) + sampler heap (linear clamp).
	D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
	heap_desc.NumDescriptors = 1;
	heap_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heap_desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(d3d_device->CreateDescriptorHeap(&heap_desc,
	                                            IID_PPV_ARGS(
	                                                    &d3d_srv_heap)))) {
		LOG_ERR("OpenXR: D3D12 SRV heap create failed");
		xrd12_shutdown();
		return false;
	}
	heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	if (FAILED(d3d_device->CreateDescriptorHeap(&heap_desc,
	                                            IID_PPV_ARGS(
	                                                    &d3d_samp_heap)))) {
		LOG_ERR("OpenXR: D3D12 sampler heap create failed");
		xrd12_shutdown();
		return false;
	}
	D3D12_SAMPLER_DESC samp = {};
	samp.Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	samp.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samp.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samp.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samp.MaxAnisotropy  = 1;
	samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	samp.MinLOD         = 0.0f;
	samp.MaxLOD         = D3D12_FLOAT32_MAX;
	d3d_device->CreateSampler(&samp,
	                         d3d_samp_heap
	                                 ->GetCPUDescriptorHandleForHeapStart());

	// CPU-only RTV heap (descriptors carved per image+layer below).
	heap_desc.NumDescriptors = 256;
	heap_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	heap_desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	if (FAILED(d3d_device->CreateDescriptorHeap(&heap_desc,
	                                            IID_PPV_ARGS(
	                                                    &d3d_rtv_heap)))) {
		LOG_ERR("OpenXR: D3D12 RTV heap create failed");
		xrd12_shutdown();
		return false;
	}
	d3d_rtv_used = 0;

	if (!compile_shaders()) {
		xrd12_shutdown();
		return false;
	}

	binding_out->type   = XR_TYPE_GRAPHICS_BINDING_D3D12_KHR;
	binding_out->next   = nullptr;
	binding_out->device = d3d_device.Get();
	binding_out->queue  = d3d_queue.Get();
	LOG_MSG("OpenXR: D3D12 binding ready%s",
	        exact ? "" : " (default adapter)");
	return true;
}

int64_t xrd12_pick_format(const int64_t* formats, const uint32_t count)
{
	d3d_swap_format  = DXGI_FORMAT_UNKNOWN;
	d3d_swap_is_bgra = false;
	if (!formats || count == 0) {
		return 0;
	}
	// hello_xr order: sRGB RGBA8 first.
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
	LOG_MSG("OpenXR: D3D12 swapchain format %lld%s",
	        static_cast<long long>(picked),
	        d3d_swap_is_bgra ? " (BGRA, R/B swapped on upload)" : "");
	return picked;
}

bool xrd12_enumerate(XrSwapchain swapchain, const char* label,
                     std::vector<XrGfxImage>& out)
{
	out.clear();
	if (!xrd12_is_ready() || swapchain == XR_NULL_HANDLE) {
		return false;
	}
	uint32_t image_count = 0;
	if (xrEnumerateSwapchainImages(swapchain, 0, &image_count, nullptr) !=
	            XR_SUCCESS ||
	    image_count == 0) {
		LOG_ERR("OpenXR: Failed to enumerate %s swapchain images", label);
		return false;
	}
	std::vector<XrSwapchainImageD3D12KHR> tmp(image_count);
	for (auto& img : tmp) {
		img.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
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
		out[i].type   = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
		out[i].handle = reinterpret_cast<uint64_t>(tmp[i].texture);
	}
	LOG_MSG("OpenXR: %s swapchain ready (%u D3D12 textures)", label,
	        image_count);
	return true;
}

// ---------------------------------------------------------------------------
// Staging + draw.
// ---------------------------------------------------------------------------

// (Re)create the upload-heap staging texture in the swapchain format
// and point the SRV slot at it.
static bool ensure_staging(const int w_px, const int h_px)
{
	if (w_px == d3d_stage_w && h_px == d3d_stage_h && d3d_stage_tex &&
	    d3d_stage_fmt == d3d_swap_format) {
		return true;
	}
	d3d_stage_tex.Reset();
	d3d_stage_w = d3d_stage_h = 0;
	d3d_stage_fmt             = DXGI_FORMAT_UNKNOWN;
	if (w_px <= 0 || h_px <= 0 ||
	    d3d_swap_format == DXGI_FORMAT_UNKNOWN) {
		return false;
	}
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_UPLOAD;
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width              = static_cast<UINT64>(w_px);
	desc.Height             = static_cast<UINT>(h_px);
	desc.DepthOrArraySize   = 1;
	desc.MipLevels          = 1;
	desc.Format             = d3d_swap_format;
	desc.SampleDesc.Count   = 1;
	desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (FAILED(d3d_device->CreateCommittedResource(
	            &heap, D3D12_HEAP_FLAG_NONE, &desc,
	            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
	            IID_PPV_ARGS(&d3d_stage_tex)))) {
		LOG_ERR("OpenXR: D3D12 staging texture create failed");
		return false;
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.Format                  = d3d_swap_format;
	srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels     = 1;
	d3d_device->CreateShaderResourceView(
	        d3d_stage_tex.Get(), &srv,
	        d3d_srv_heap->GetCPUDescriptorHandleForHeapStart());
	d3d_stage_w   = w_px;
	d3d_stage_h   = h_px;
	d3d_stage_fmt = d3d_swap_format;
	return true;
}

// Map + convert bottom-up source rows into the staging texture
// (top-down, respecting the mapped row pitch).
static bool fill_staging(const uint32_t* src_bottom_up, const int w_px,
                          const int h_px, const bool src_is_bgra)
{
	void* mapped = nullptr;
	if (FAILED(d3d_stage_tex->Map(0, nullptr, &mapped)) || !mapped) {
		LOG_ERR("OpenXR: D3D12 staging map failed");
		return false;
	}
	D3D12_RESOURCE_DESC desc = d3d_stage_tex->GetDesc();
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	UINT num_rows = 0;
	UINT64 row_bytes = 0, total_bytes = 0;
	d3d_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint,
	                                  &num_rows, &row_bytes,
	                                  &total_bytes);
	const size_t pitch = footprint.Footprint.RowPitch;
	const bool swap = (src_is_bgra != d3d_swap_is_bgra);
	for (int y = 0; y < h_px; ++y) {
		const uint32_t* srow =
		        src_bottom_up +
		        static_cast<size_t>(h_px - 1 - y) * w_px;
		uint8_t* drow = static_cast<uint8_t*>(mapped) +
		                static_cast<size_t>(y) * pitch;
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
	D3D12_RANGE written = {0, static_cast<SIZE_T>(pitch) * h_px};
	d3d_stage_tex->Unmap(0, &written);
	return true;
}

static D3D12_CPU_DESCRIPTOR_HANDLE get_rtv(const uint64_t handle,
                                           const int layer)
{
	const auto key = rtv_key(handle, layer);
	const auto it  = d3d_rtvs.find(key);
	if (it != d3d_rtvs.end()) {
		return it->second;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
	if (d3d_rtv_used >= 256) {
		LOG_ERR("OpenXR: D3D12 RTV heap exhausted");
		return rtv;
	}
	const UINT stride = d3d_device->GetDescriptorHandleIncrementSize(
	        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	rtv = d3d_rtv_heap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += static_cast<SIZE_T>(d3d_rtv_used) * stride;
	++d3d_rtv_used;
	ID3D12Resource* tex = reinterpret_cast<ID3D12Resource*>(
	        static_cast<uintptr_t>(handle));
	D3D12_RENDER_TARGET_VIEW_DESC desc = {};
	desc.Format        = d3d_swap_format;
	desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
	desc.Texture2DArray.MipSlice        = 0;
	desc.Texture2DArray.FirstArraySlice = static_cast<UINT>(layer);
	desc.Texture2DArray.ArraySize       = 1;
	d3d_device->CreateRenderTargetView(tex, &desc, rtv);
	d3d_rtvs[key] = rtv;
	return rtv;
}

// Record + execute a clear + textured-quad draw. The viewport maps
// the full source texture into the (possibly off-image) dst rect;
// the scissor clips rasterization to the image. blend selects the
// alpha-blend PSO (overlays) vs opaque (projection).
static bool draw_quad(D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                      const int vp_x_px, const int vp_y_px,
                      const int vp_w_px, const int vp_h_px,
                      const int sc_x_px, const int sc_y_px,
                      const int sc_w_px, const int sc_h_px,
                      const bool blend)
{
	if (FAILED(d3d_alloc->Reset()) ||
	    FAILED(d3d_list->Reset(d3d_alloc.Get(), nullptr))) {
		return false;
	}
	d3d_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	static constexpr float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	d3d_list->ClearRenderTargetView(rtv, black, 0, nullptr);
	if (vp_w_px > 0 && vp_h_px > 0 && sc_w_px > 0 && sc_h_px > 0) {
		D3D12_VIEWPORT vp = {};
		vp.TopLeftX = static_cast<FLOAT>(vp_x_px);
		vp.TopLeftY = static_cast<FLOAT>(vp_y_px);
		vp.Width    = static_cast<FLOAT>(vp_w_px);
		vp.Height   = static_cast<FLOAT>(vp_h_px);
		vp.MinDepth = 0.0f;
		vp.MaxDepth = 1.0f;
		d3d_list->RSSetViewports(1, &vp);
		const D3D12_RECT scissor = {sc_x_px, sc_y_px, sc_x_px + sc_w_px,
		                            sc_y_px + sc_h_px};
		d3d_list->RSSetScissorRects(1, &scissor);
		d3d_list->SetPipelineState(blend ? d3d_pso_blend.Get()
		                                 : d3d_pso_opaque.Get());
		d3d_list->SetGraphicsRootSignature(d3d_root_sig.Get());
		ID3D12DescriptorHeap* heaps[] = {d3d_srv_heap.Get(),
		                                 d3d_samp_heap.Get()};
		d3d_list->SetDescriptorHeaps(2, heaps);
		d3d_list->SetGraphicsRootDescriptorTable(
		        0, d3d_srv_heap->GetGPUDescriptorHandleForHeapStart());
		d3d_list->SetGraphicsRootDescriptorTable(
		        1, d3d_samp_heap->GetGPUDescriptorHandleForHeapStart());
		d3d_list->IASetPrimitiveTopology(
		        D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		d3d_list->DrawInstanced(3, 1, 0, 0);
	}
	if (FAILED(d3d_list->Close())) {
		return false;
	}
	ID3D12CommandList* lists[] = {
	        static_cast<ID3D12CommandList*>(d3d_list.Get())};
	d3d_queue->ExecuteCommandLists(1, lists);
	++d3d_fence_value;
	if (FAILED(d3d_queue->Signal(d3d_fence.Get(), d3d_fence_value))) {
		return false;
	}
	if (d3d_fence->GetCompletedValue() < d3d_fence_value) {
		if (FAILED(d3d_fence->SetEventOnCompletion(d3d_fence_value,
		                                           d3d_fence_event))) {
			return false;
		}
		static constexpr DWORD FenceWaitMs = 100;
		if (WaitForSingleObject(d3d_fence_event, FenceWaitMs) !=
		    WAIT_OBJECT_0) {
			LOG_ERR("OpenXR: D3D12 fence wait timed out");
			xrd12_shutdown();
			return false;
		}
	}
	return true;
}

static bool upload_pixels(const uint64_t dst_handle, const int dst_layer,
                          const int eye_w_px, const int eye_h_px,
                          const int dx_px, const int dy_px_top,
                          const int dw_px, const int dh_px,
                          const uint32_t* px_bottom_up, const int src_w_px,
                          const int src_h_px, const bool src_is_bgra,
                          const bool blend)
{
	// Clip the scissor to the image (the disparity shift can push it
	// past the edge); the viewport keeps the unclipped rect so the
	// full texture still maps onto it.
	int sc_x0 = dx_px, sc_y0 = dy_px_top;
	int sc_x1 = dx_px + dw_px, sc_y1 = dy_px_top + dh_px;
	if (sc_x0 < 0) {
		sc_x0 = 0;
	}
	if (sc_y0 < 0) {
		sc_y0 = 0;
	}
	if (sc_x1 > eye_w_px) {
		sc_x1 = eye_w_px;
	}
	if (sc_y1 > eye_h_px) {
		sc_y1 = eye_h_px;
	}
	if (!ensure_staging(src_w_px, src_h_px)) {
		return false;
	}
	if (!fill_staging(px_bottom_up, src_w_px, src_h_px, src_is_bgra)) {
		return false;
	}
	const auto rtv = get_rtv(dst_handle, dst_layer);
	if (rtv.ptr == 0) {
		return false;
	}
	return draw_quad(rtv, dx_px, dy_px_top, dw_px, dh_px, sc_x0, sc_y0,
	                 sc_x1 - sc_x0, sc_y1 - sc_y0, blend);
}

bool xrd12_upload_full(const uint64_t dst_handle, const int dst_layer,
                       const int w_px, const int h_px,
                       const uint32_t* rgba_bottom_up)
{
	if (!xrd12_is_ready() || !dst_handle || !rgba_bottom_up ||
	    w_px <= 0 || h_px <= 0 ||
	    d3d_swap_format == DXGI_FORMAT_UNKNOWN) {
		return false;
	}
	// Rasters are RGBA-ordered; the staging texture matches the
	// swapchain, so no lane swap unless it wants BGRA.
	return upload_pixels(dst_handle, dst_layer, w_px, h_px, 0, 0, w_px,
	                     h_px, rgba_bottom_up, w_px, h_px, false, true);
}

bool xrd12_upload_fitted(const uint64_t dst_handle, const int dst_layer,
                         const int eye_w_px, const int eye_h_px,
                         const int dx_px, const int dy_px_top,
                         const int dw_px, const int dh_px,
                         const uint32_t* bgra_bottom_up,
                         const int src_w_px, const int src_h_px)
{
	if (!xrd12_is_ready() || !dst_handle || !bgra_bottom_up ||
	    eye_w_px <= 0 || eye_h_px <= 0 || src_w_px <= 0 ||
	    src_h_px <= 0 || d3d_swap_format == DXGI_FORMAT_UNKNOWN) {
		return false;
	}
	return upload_pixels(dst_handle, dst_layer, eye_w_px, eye_h_px,
	                     dx_px, dy_px_top, dw_px, dh_px, bgra_bottom_up,
	                     src_w_px, src_h_px, true, false);
}

#endif // C_XR_D3D12
