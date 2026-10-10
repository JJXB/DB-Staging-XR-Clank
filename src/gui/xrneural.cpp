// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Neural monocular depth (see xrneural.h): ONNX Runtime loader,
// hosted-model registry, background worker, and depth-map cache.

#include "private/sdl_gui.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <string>

#if C_OPENXR
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>
#if defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

#if defined(_WIN32)
// openxr_platform.h needs the Win32/GDI/COM types visible first.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>
#endif

#define XR_USE_GRAPHICS_API_OPENGL
#if defined(_WIN32)
#define XR_USE_PLATFORM_WIN32
#elif defined(__linux__)
#define XR_USE_PLATFORM_XLIB
#define XR_USE_PLATFORM_WAYLAND
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#endif

#include "private/common.h"

#include "audio/mixer.h"
#include "capture/capture.h"
#include "config/config.h"
#include "config/setup.h"
#include "cpu/cpu.h"
#include "dosbox.h"
#include "gui/mapper.h"
#include "gui/render/opengl_renderer.h"
#include "gui/render/sdl_renderer.h"
#include "gui/titlebar.h"
#include "gui/xr.h"
#include "gui/osd.h"
#include "gui/xrneural.h"
#include "hardware/input/keyboard.h"
#include "hardware/input/joystick.h"
#include "hardware/input/mouse.h"
#include "hardware/port.h"
#include "hardware/timer.h"
#include "hardware/video/vga.h"
#include "misc/cross.h"
#include "misc/notifications.h"
#include "misc/support.h"
#include "misc/video.h"
#include "utils/checks.h"
#include "utils/env_utils.h"
#include "utils/math_utils.h"
#include "utils/rect.h"
#include "utils/string_utils.h"

// must be included after dosbox_config.h
#include <SDL3/SDL.h>

CHECK_NARROWING();

#if C_OPENXR
#if C_NEURAL
bool xr_autodepth_wants_neural(const std::string& method)
{
	return method == "neural" || method == "neural+gradient" ||
	       method == "neural+luma" || method == "neural+blend";
}

bool xr_autodepth_is_fusion(const std::string& method)
{
	return method == "neural+gradient" || method == "neural+luma" ||
	       method == "neural+blend";
}
#else
bool xr_autodepth_wants_neural(const std::string&)
{
	return false;
}

bool xr_autodepth_is_fusion(const std::string&)
{
	return false;
}
#endif

#if C_NEURAL
// ---------------------------------------------------------------------------
// Neural monocular depth ('neural' autodepth submode)
// ---------------------------------------------------------------------------
// MiDaS v2.1 small (256x256 input, normalization baked into the model)
// via a runtime-loaded ONNX Runtime: no build-time dependency, nothing
// links against ORT. Files live in <config>/xrdepth/ (filenames kept in
// sync with dos/programs/xrdepth.cpp):
//   midas_v21_small_256.onnx  (~66 MB model)
//   onnxruntime.dll           (plus onnxruntime_providers_shared.dll, if any)
// A background worker runs inference about once a second; the blit path
// reuses the latest map and falls back to the heuristic cues when none
// is ready, so neural mode degrades gracefully instead of stalling.
//
// ABI notes: the declarations below mirror onnxruntime_c_api.h v1.30.0
// (verified member-by-member against the upstream header) for the prefix
// through ReleaseValue; unneeded slots are padding (every member is one
// pointer). OrtApi is append-only across versions, so requesting API v13
// keeps every used slot aligned on any runtime >= 1.13.
#if defined(_WIN32)
#define XR_ORT_CALL __stdcall
#else
#define XR_ORT_CALL
#endif
#if defined(_WIN32)
using XrOrtChar = wchar_t;
#else
using XrOrtChar = char;
#endif

struct OrtEnv;
struct OrtSession;
struct OrtSessionOptions;
struct OrtRunOptions;
struct OrtValue;
struct OrtTypeInfo;
struct OrtTensorTypeAndShapeInfo;
struct OrtMemoryInfo;
struct OrtAllocator;
struct OrtStatus;

enum XrOrtErrorCode { XR_ORT_OK = 0 };
enum XrOrtLoggingLevel {
	XR_ORT_LOGGING_VERBOSE = 0,
	XR_ORT_LOGGING_INFO    = 1,
	XR_ORT_LOGGING_WARNING = 2,
	XR_ORT_LOGGING_ERROR   = 3,
	XR_ORT_LOGGING_FATAL   = 4
};
enum XrOrtGraphOptimizationLevel {
	XR_ORT_DISABLE_ALL = 0,
	XR_ORT_ENABLE_BASIC = 1,
	XR_ORT_ENABLE_EXTENDED = 2,
	XR_ORT_ENABLE_ALL = 99
};
enum XrOrtAllocatorType { XR_ORT_DEVICE_ALLOCATOR = 0 };
enum XrOrtMemType { XR_ORT_MEM_DEFAULT = 0 };
enum XrOrtTensorElementDataType {
	XR_ORT_ELEM_UNDEFINED = 0,
	XR_ORT_ELEM_FLOAT = 1,
	XR_ORT_ELEM_UINT8 = 2,
	XR_ORT_ELEM_INT8 = 3
};

struct XrOrtApiBase {
	const struct XrOrtApi* (XR_ORT_CALL* GetApi)(uint32_t version) noexcept;
	const char* (XR_ORT_CALL* GetVersionString)(void) noexcept;
};

struct XrOrtApi {
	void* CreateStatus_; // 1
	XrOrtErrorCode(XR_ORT_CALL* GetErrorCode)(const OrtStatus* status) noexcept; // 2
	const char*(XR_ORT_CALL* GetErrorMessage)(const OrtStatus* status) noexcept; // 3
	OrtStatus*(XR_ORT_CALL* CreateEnv)(XrOrtLoggingLevel log_severity_level,
	                                   const char* logid,
	                                   OrtEnv** out) noexcept; // 4
	void* CreateEnvWithCustomLogger_; // 5
	void* EnableTelemetryEvents_; // 6
	void* DisableTelemetryEvents_; // 7
	OrtStatus*(XR_ORT_CALL* CreateSession)(const OrtEnv* env,
	                                       const XrOrtChar* model_path,
	                                       const OrtSessionOptions* options,
	                                       OrtSession** out) noexcept; // 8
	void* CreateSessionFromArray_; // 9
	OrtStatus*(XR_ORT_CALL* Run)(OrtSession* session,
	                             const OrtRunOptions* run_options,
	                             const char* const* input_names,
	                             const OrtValue* const* inputs,
	                             size_t input_len,
	                             const char* const* output_names,
	                             size_t output_names_len,
	                             OrtValue** outputs) noexcept; // 10
	OrtStatus*(XR_ORT_CALL* CreateSessionOptions)(OrtSessionOptions** options) noexcept; // 11
	void* SetOptimizedModelFilePath_; // 12
	void* CloneSessionOptions_; // 13
	void* SetSessionExecutionMode_; // 14
	void* EnableProfiling_; // 15
	void* DisableProfiling_; // 16
	void* EnableMemPattern_; // 17
	void* DisableMemPattern_; // 18
	void* EnableCpuMemArena_; // 19
	void* DisableCpuMemArena_; // 20
	void* SetSessionLogId_; // 21
	void* SetSessionLogVerbosityLevel_; // 22
	void* SetSessionLogSeverityLevel_; // 23
	OrtStatus*(XR_ORT_CALL* SetSessionGraphOptimizationLevel)(OrtSessionOptions* options,
	                                                          XrOrtGraphOptimizationLevel graph_optimization_level) noexcept; // 24
	OrtStatus*(XR_ORT_CALL* SetIntraOpNumThreads)(OrtSessionOptions* options,
	                                              int intra_op_num_threads) noexcept; // 25
	void* SetInterOpNumThreads_; // 26
	void* CreateCustomOpDomain_; // 27
	void* CustomOpDomain_Add_; // 28
	void* AddCustomOpDomain_; // 29
	void* RegisterCustomOpsLibrary_; // 30
	OrtStatus*(XR_ORT_CALL* SessionGetInputCount)(const OrtSession* session,
	                                              size_t* out) noexcept; // 31
	OrtStatus*(XR_ORT_CALL* SessionGetOutputCount)(const OrtSession* session,
	                                               size_t* out) noexcept; // 32
	void* SessionGetOverridableInitializerCount_; // 33
	OrtStatus*(XR_ORT_CALL* SessionGetInputTypeInfo)(const OrtSession* session,
	                                                 size_t index,
	                                                 OrtTypeInfo** type_info) noexcept; // 34
	OrtStatus*(XR_ORT_CALL* SessionGetOutputTypeInfo)(const OrtSession* session,
	                                                  size_t index,
	                                                  OrtTypeInfo** type_info) noexcept; // 35
	void* SessionGetOverridableInitializerTypeInfo_; // 36
	OrtStatus*(XR_ORT_CALL* SessionGetInputName)(const OrtSession* session,
	                                             size_t index,
	                                             OrtAllocator* allocator,
	                                             char** value) noexcept; // 37
	OrtStatus*(XR_ORT_CALL* SessionGetOutputName)(const OrtSession* session,
	                                              size_t index,
	                                              OrtAllocator* allocator,
	                                              char** value) noexcept; // 38
	void* SessionGetOverridableInitializerName_; // 39
	void* CreateRunOptions_; // 40
	void* RunOptionsSetRunLogVerbosityLevel_; // 41
	void* RunOptionsSetRunLogSeverityLevel_; // 42
	void* RunOptionsSetRunTag_; // 43
	void* RunOptionsGetRunLogVerbosityLevel_; // 44
	void* RunOptionsGetRunLogSeverityLevel_; // 45
	void* RunOptionsGetRunTag_; // 46
	void* RunOptionsSetTerminate_; // 47
	void* RunOptionsUnsetTerminate_; // 48
	void* CreateTensorAsOrtValue_; // 49
	OrtStatus*(XR_ORT_CALL* CreateTensorWithDataAsOrtValue)(const OrtMemoryInfo* info,
	                                                        void* p_data,
	                                                        size_t p_data_len,
	                                                        const int64_t* shape,
	                                                        size_t shape_len,
	                                                        XrOrtTensorElementDataType type,
	                                                        OrtValue** out) noexcept; // 50
	void* IsTensor_; // 51
	OrtStatus*(XR_ORT_CALL* GetTensorMutableData)(OrtValue* value,
	                                              void** out) noexcept; // 52
	void* FillStringTensor_; // 53
	void* GetStringTensorDataLength_; // 54
	void* GetStringTensorContent_; // 55
	OrtStatus*(XR_ORT_CALL* CastTypeInfoToTensorInfo)(const OrtTypeInfo* type_info,
	                                                  const OrtTensorTypeAndShapeInfo** out) noexcept; // 56
	void* GetOnnxTypeFromTypeInfo_; // 57
	void* CreateTensorTypeAndShapeInfo_; // 58
	void* SetTensorElementType_; // 59
	void* SetDimensions_; // 60
	OrtStatus*(XR_ORT_CALL* GetTensorElementType)(const OrtTensorTypeAndShapeInfo* info,
	                                              XrOrtTensorElementDataType* out) noexcept; // 61
	OrtStatus*(XR_ORT_CALL* GetDimensionsCount)(const OrtTensorTypeAndShapeInfo* info,
	                                            size_t* out) noexcept; // 62
	OrtStatus*(XR_ORT_CALL* GetDimensions)(const OrtTensorTypeAndShapeInfo* info,
	                                       int64_t* dim_values,
	                                       size_t dim_values_length) noexcept; // 63
	void* GetSymbolicDimensions_; // 64
	void* GetTensorShapeElementCount_; // 65
	void* GetTensorTypeAndShape_; // 66
	void* GetTypeInfo_; // 67
	void* GetValueType_; // 68
	void* CreateMemoryInfo_; // 69
	OrtStatus*(XR_ORT_CALL* CreateCpuMemoryInfo)(XrOrtAllocatorType type,
	                                             XrOrtMemType mem_type,
	                                             OrtMemoryInfo** out) noexcept; // 70
	void* CompareMemoryInfo_; // 71
	void* MemoryInfoGetName_; // 72
	void* MemoryInfoGetId_; // 73
	void* MemoryInfoGetMemType_; // 74
	void* MemoryInfoGetType_; // 75
	void* AllocatorAlloc_; // 76
	OrtStatus*(XR_ORT_CALL* AllocatorFree)(OrtAllocator* ort_allocator,
	                                       void* p) noexcept; // 77
	void* AllocatorGetInfo_; // 78
	OrtStatus*(XR_ORT_CALL* GetAllocatorWithDefaultOptions)(OrtAllocator** out) noexcept; // 79
	void* AddFreeDimensionOverride_; // 80
	void* GetValue_; // 81
	void* GetValueCount_; // 82
	void* CreateValue_; // 83
	void* CreateOpaqueValue_; // 84
	void* GetOpaqueValue_; // 85
	void* KernelInfoGetAttribute_float_; // 86
	void* KernelInfoGetAttribute_int64_; // 87
	void* KernelInfoGetAttribute_string_; // 88
	void* KernelContext_GetInputCount_; // 89
	void* KernelContext_GetOutputCount_; // 90
	void* KernelContext_GetInput_; // 91
	void* KernelContext_GetOutput_; // 92
	void* ReleaseEnv_; // 93 (process-lifetime objects are never released)
	void (XR_ORT_CALL* ReleaseStatus)(OrtStatus* input); // 94
	void* ReleaseMemoryInfo_; // 95
	void (XR_ORT_CALL* ReleaseSession)(OrtSession* input); // 96
	void (XR_ORT_CALL* ReleaseValue)(OrtValue* input); // 97
};

using XrOrtGetApiBaseFn =
        const XrOrtApiBase* (XR_ORT_CALL*)(void) noexcept;

// Neural depth model registry. Inference parameters live here; file
// identity (id/file) is mirrored in dos/programs/xrdepth.cpp, which
// owns the download URLs.
struct XrNeuralModelDef {
	const char* id;      // config + toggle name
	const char* file;    // onnx filename in the xrdepth dir
	bool imagenet_norm;  // else plain [0,1] input (float inputs only)
	bool invert_output;  // raw near reads low (metric-style depth)
	int64_t def_w;       // fallback input size for dynamic spatial dims
	int64_t def_h;
};
static constexpr XrNeuralModelDef xr_neural_models[] = {
        {"midas", "midas_v21_small_256.onnx", false, false, 256, 256},
        // DA2 is fully dynamic (any H/W, ideally a multiple of the
        // patch size 14); 518 is its canonical size.
        {"da2int8", "depth_anything_v2_small_int8.onnx", true, false, 518, 518},
        {"fastdepth", "fastdepth.onnx", true, true, 224, 224},
        // ResNet18 U-Net (NYU meters, ImageNet norm). No hosted weights
        // file exists; convert the PyTorch checkpoint locally (see the
        // export script kept with the dev notes) and drop the result
        // here as lightdepth.onnx. Fully convolutional: any size goes.
        {"lightdepth", "lightdepth.onnx", true, true, 256, 256},
};
static constexpr int xr_neural_model_count =
        sizeof(xr_neural_models) / sizeof(xr_neural_models[0]);

static const XrNeuralModelDef* xr_neural_find(const std::string& id)
{
	for (const auto& def : xr_neural_models) {
		if (id == def.id) {
			return &def;
		}
	}
	return &xr_neural_models[0];
}

// Neural depth runtime state (process-lifetime; the worker owns the
// ORT session and only idles when neural mode is off).
struct XrNeuralState {
#if defined(_WIN32)
	HMODULE lib = nullptr;
#else
	void* lib = nullptr;
#endif
	const XrOrtApi* ort           = nullptr;
	OrtEnv* env                   = nullptr;
	OrtSession* session           = nullptr;
	OrtSessionOptions* options    = nullptr;
	OrtMemoryInfo* mem            = nullptr;
	OrtAllocator* alloc           = nullptr;
	std::string input_name;
	std::string output_name;
	int64_t in_w = 0, in_h = 0; // model input spatial dims
	bool input_nhwc = false; // else NCHW
	int in_elem_type = 0; // ONNX element enum (1 = float)
	std::vector<uint8_t> in_tensor_u8; // staging for quantized inputs
	int64_t out_w = 0, out_h = 0; // model output plane dims
	int out_elem_type = 0;
	std::vector<float> in_tensor; // RGB input, model layout
	std::string model_id; // loaded model id (empty = none)
	std::string request_model; // wanted model id (toggle/config)
	bool invert_output = false; // current model reads near-low
	bool imagenet_norm = false; // current model wants ImageNet norm
	int64_t in_shape_dims[4] = {1, 3, 0, 0};
	bool runtime_ok = false;
	std::thread worker;
	std::mutex mtx;
	std::condition_variable cv;
	bool stop_requested  = false;
	bool worker_running  = false;
	bool load_done       = false; // loader ran (success or terminal state below)
	bool ready           = false; // session usable
	std::string error; // last loader error for status/toasts
	bool has_job  = false;
	bool job_busy = false;
	std::vector<uint32_t> job_frame; // bottom-up ARGB32 source frame
	int job_w = 0, job_h = 0;
	uint64_t submit_hash       = 0; // frame sample hash, stage-A skip
	bool submit_hash_valid     = false;
	uint64_t tensor_hash       = 0; // preprocessed input hash, stage-B
	bool tensor_hash_valid     = false;
	uint64_t submit_skipped    = 0; // identical-frame submits avoided
	uint64_t inference_skipped = 0; // identical-tensor runs avoided
	std::vector<float> map; // published model-space inverse-depth map
	int map_w = 0, map_h = 0;
	uint64_t map_epoch = 0;
	int64_t last_attempt_us = 0;
	int64_t last_submit_us  = 0;
	bool toast_loading = false;
	bool toast_active  = false;
	std::string toast_needed; // worker asks main thread to warn once
	bool toast_done = false;
};

static XrNeuralState xr_neural;
static std::once_flag xr_neural_stop_flag;

// FNV-1a 64-bit (frame/tensor change detection below).
static uint64_t xr_fnv1a(const void* data, size_t len, uint64_t seed)
{
	const auto* bytes = static_cast<const uint8_t*>(data);
	uint64_t h        = seed;
	for (size_t i = 0; i < len; ++i) {
		h ^= bytes[i];
		h *= 1099511628211ULL;
	}
	return h;
}

// Keep in sync with dos/programs/xrdepth.cpp.
static std_fs::path xr_neural_dir()
{
	return get_config_dir() / "xrdepth";
}
static std_fs::path xr_neural_dll_path()
{
#if defined(_WIN32)
	return xr_neural_dir() / "onnxruntime.dll";
#elif defined(__linux__)
	return xr_neural_dir() / "libonnxruntime.so";
#elif defined(__APPLE__)
	return xr_neural_dir() / "libonnxruntime.dylib";
#else
	return std_fs::path();
#endif
}

// Short element-type name for diagnostics (1=float, 2=uint8, 3=int8).
static const char* xr_ort_elem_name(const int elem)
{
	switch (elem) {
	case 1: return "float";
	case 2: return "uint8";
	case 3: return "int8";
	default: return "?";
	}
}

// Rank-aware dim formatting for the spec log ([1,3,-1,-1], no padding
// past the rank).
static std::string xr_ort_dims_str(const int64_t* dims, size_t rank)
{
	std::string s = "[";
	if (rank > 8) {
		rank = 8;
	}
	for (size_t i = 0; i < rank; ++i) {
		if (i) {
			s += ",";
		}
		s += std::to_string(dims[i]);
	}
	return s + "]";
}

// Silent status check for hot paths: hands the message out for
// throttled logging instead of flooding every inference.
static bool xr_ort_status_ok(OrtStatus* st, std::string& msg_out)
{
	if (!st) {
		return true;
	}
	auto* ort = xr_neural.ort;
	if (!ort) {
		msg_out = "?";
		return false; // leaks st; unreachable after a good load
	}
	msg_out = ort->GetErrorMessage(st);
	ort->ReleaseStatus(st);
	return false;
}

// Report an ORT failure (logs the message, releases the status).
static bool xr_ort_check(OrtStatus* st, const char* what)
{
	if (!st) {
		return true;
	}
	const char* msg = "?";
	if (xr_neural.ort) {
		msg = xr_neural.ort->GetErrorMessage(st);
	}
	LOG_ERR("OpenXR: Neural depth %s failed: %s", what, msg);
	xr_neural.ort->ReleaseStatus(st);
	return false;
}

static void* xr_ort_load_library(const std_fs::path& path)
{
#if defined(_WIN32)
	// The DLL's own directory must resolve its sibling dependencies.
	wchar_t prev_dir[MAX_PATH] = {};
	const DWORD prev_len = GetDllDirectoryW(ARRAYSIZE(prev_dir), prev_dir);
	const std::wstring dir = path.parent_path().wstring();
	SetDllDirectoryW(dir.c_str());
	HMODULE mod = LoadLibraryW(path.wstring().c_str());
	if (prev_len > 0) {
		SetDllDirectoryW(prev_dir);
	} else {
		SetDllDirectoryW(nullptr);
	}
	return mod;
#elif defined(__linux__) || defined(__APPLE__)
	return dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
#else
	(void)path;
	return nullptr;
#endif
}

// Runtime setup once on the worker thread (library, API, env, options,
// allocator). Model sessions come and go on top of it.
static bool xr_neural_ensure_runtime()
{
	auto& s = xr_neural;
	if (s.runtime_ok) {
		return true;
	}
	const auto dll_path = xr_neural_dll_path();
	if (dll_path.empty() || !std_fs::exists(dll_path)) {
		s.error = "onnxruntime library missing (run XRDEPTH DOWNLOAD)";
		return false;
	}
	s.lib = static_cast<decltype(s.lib)>(
	        xr_ort_load_library(dll_path));
	if (!s.lib) {
		s.error = "could not load onnxruntime library";
		return false;
	}
#if defined(_WIN32)
	// Via void* to avoid function-type-mismatch warnings; the signature
	// mirrors OrtGetApiBase in onnxruntime_c_api.h (verified).
	auto* get_base = reinterpret_cast<XrOrtGetApiBaseFn>(
	        reinterpret_cast<void*>(
	                GetProcAddress(s.lib, "OrtGetApiBase")));
#else
	auto* get_base = reinterpret_cast<XrOrtGetApiBaseFn>(
	        reinterpret_cast<void*>(dlsym(s.lib, "OrtGetApiBase")));
#endif
	if (!get_base) {
		s.error = "OrtGetApiBase not exported";
		return false;
	}
	const XrOrtApiBase* base = get_base();
	if (!base || !base->GetApi) {
		s.error = "bad OrtApiBase";
		return false;
	}
	// API v13: every function below exists there; the struct prefix
	// matches any runtime >= 1.13 (append-only ABI).
	s.ort = base->GetApi(13);
	if (!s.ort) {
		s.error = "ORT API v13 unsupported by this runtime";
		return false;
	}
	LOG_MSG("OpenXR: Neural depth runtime %s", base->GetVersionString());

	auto* ort = s.ort;
	if (!xr_ort_check(ort->CreateEnv(XR_ORT_LOGGING_WARNING,
	                                "dosbox-xr",
	                                &s.env),
	                  "CreateEnv") ||
	    !xr_ort_check(ort->CreateSessionOptions(&s.options),
	                  "CreateSessionOptions")) {
		s.error = "env/options setup failed";
		return false;
	}
	// Best effort; defaults apply when the runtime ignores them.
	xr_ort_check(ort->SetSessionGraphOptimizationLevel(
	                     s.options, XR_ORT_ENABLE_ALL),
	             "graph optimization");
	unsigned threads = std::thread::hardware_concurrency();
	if (threads < 1) {
		threads = 2;
	}
	if (threads > 4) {
		threads = 4;
	}
	// Manual cap keeps inference off the emulator's cores ('0' keeps
	// the automatic choice above).
	const int want_threads = get_xr_section()->GetInt("xr_neural_threads");
	if (want_threads >= 1 && want_threads <= 16) {
		threads = static_cast<unsigned>(want_threads);
	}
	xr_ort_check(ort->SetIntraOpNumThreads(s.options,
	                                       static_cast<int>(threads)),
	             "thread count");
	LOG_MSG("OpenXR: Neural depth worker threads: %u", threads);
	if (!xr_ort_check(ort->GetAllocatorWithDefaultOptions(&s.alloc),
	                  "default allocator") ||
	    !xr_ort_check(ort->CreateCpuMemoryInfo(XR_ORT_DEVICE_ALLOCATOR,
	                                           XR_ORT_MEM_DEFAULT,
	                                           &s.mem),
	                  "cpu memory info")) {
		s.error = "allocator setup failed";
		return false;
	}
	s.runtime_ok = true;
	return true;
}

// Drop the current model session (worker thread, between jobs). The
// published map is invalidated so frames fall back until the next map.
static void xr_neural_unload_model()
{
	auto& s = xr_neural;
	{
		std::lock_guard<std::mutex> lock(s.mtx);
		s.ready      = false;
		s.map_epoch  = 0;
		s.model_id.clear();
		// New model, new outputs: identical inputs must re-run.
		s.submit_hash_valid = false;
		s.tensor_hash_valid = false;
	}
	if (s.session) {
		s.ort->ReleaseSession(s.session);
		s.session = nullptr;
	}
	s.input_name.clear();
	s.output_name.clear();
}

// Full model session setup on the worker thread (slow: multi-second
// load). Unloads any previous model first.
static bool xr_neural_load_model(const XrNeuralModelDef* def)
{
	auto& s = xr_neural;
	if (!def) {
		def = &xr_neural_models[0];
	}
	{
		std::lock_guard<std::mutex> lock(s.mtx);
		if (s.ready && s.model_id == def->id) {
			return true;
		}
	}
	xr_neural_unload_model();
	const auto model_path = xr_neural_dir() / def->file;
	if (!std_fs::exists(model_path)) {
		s.error = std::string("model file missing: ") + def->file;
		return false;
	}
	if (!xr_neural_ensure_runtime() || !s.ort) {
		if (s.error.empty()) {
			s.error = "runtime setup failed";
		}
		return false;
	}
	auto* ort = s.ort;
#if defined(_WIN32)
	const std::wstring wmodel = model_path.wstring();
	if (!xr_ort_check(ort->CreateSession(s.env,
	                                    wmodel.c_str(),
	                                    s.options,
	                                    &s.session),
	                  "CreateSession")) {
#else
	if (!xr_ort_check(ort->CreateSession(s.env,
	                                    model_path.string().c_str(),
	                                    s.options,
	                                    &s.session),
	                  "CreateSession")) {
#endif
		s.error = "model load failed (bad or corrupt file?)";
		return false;
	}
	size_t in_count = 0, out_count = 0;
	if (!xr_ort_check(ort->SessionGetInputCount(s.session, &in_count),
	                  "input count") ||
	    !xr_ort_check(ort->SessionGetOutputCount(s.session, &out_count),
	                  "output count") ||
	    in_count < 1 || out_count < 1) {
		s.error = "model has no inputs/outputs";
		return false;
	}
	// Input: float rank-4 with a channel dim of 3 (NCHW or NHWC).
	char* in_name = nullptr;
	OrtTypeInfo* in_type = nullptr;
	bool ok = xr_ort_check(ort->SessionGetInputName(s.session,
	                                                0,
	                                                s.alloc,
	                                                &in_name),
	                       "input name") &&
	          xr_ort_check(ort->SessionGetInputTypeInfo(s.session,
	                                                     0,
	                                                     &in_type),
	                       "input type");
	if (ok && in_name) {
		s.input_name = in_name;
	}
	if (in_name) {
		ort->AllocatorFree(s.alloc, in_name);
	}
	const OrtTensorTypeAndShapeInfo* in_shape = nullptr;
	XrOrtTensorElementDataType in_elem = XR_ORT_ELEM_UNDEFINED;
	size_t in_rank = 0;
	int64_t in_dims[8] = {};
	ok = ok &&
	     xr_ort_check(ort->CastTypeInfoToTensorInfo(in_type, &in_shape),
	                  "input shape cast") &&
	     xr_ort_check(ort->GetTensorElementType(in_shape, &in_elem),
	                  "input element type") &&
	     xr_ort_check(ort->GetDimensionsCount(in_shape, &in_rank),
	                  "input rank") &&
	     (in_rank == 4) &&
	     xr_ort_check(ort->GetDimensions(in_shape, in_dims, in_rank),
	                  "input dims");
	if (!ok || in_rank != 4) {
		s.error = "unexpected model input (want rank-4)";
		LOG_MSG("OpenXR: Neural depth %s input spec: name '%s' rank %llu elem %s",
		        def->id,
		        s.input_name.c_str(),
		        (unsigned long long)in_rank,
		        xr_ort_elem_name(in_elem));
		return false;
	}
	// Float natively; quantized inputs are fed raw bytes ([0,1] range).
	const bool in_float = (in_elem == XR_ORT_ELEM_FLOAT);
	const bool in_bytes = (in_elem == XR_ORT_ELEM_UINT8 ||
	                       in_elem == XR_ORT_ELEM_INT8);
	if (!in_float && !in_bytes) {
		s.error = "unexpected model input element type";
		LOG_MSG("OpenXR: Neural depth %s input spec: name '%s' elem %s",
		        def->id,
		        s.input_name.c_str(),
		        xr_ort_elem_name(in_elem));
		return false;
	}
	int64_t batch = in_dims[0];
	if (batch <= 0) {
		// Symbolic/dynamic batch: assume 1 (we only feed one frame).
		LOG_MSG("OpenXR: Neural depth %s: dynamic batch dim, assuming 1",
		        def->id);
		batch = 1;
	}
	if (batch != 1) {
		s.error = "model batch must be 1";
		return false;
	}
	// NCHW [1,3,H,W] or NHWC [1,H,W,3]. Symbolic spatial dims
	// (fully dynamic models like DA2) fall back to the registry
	// default; feeding a concrete shape to symbolic dims is legal.
	int64_t ih = 0, iw = 0;
	bool nhwc = false;
	if (in_dims[1] == 3) {
		ih = in_dims[2];
		iw = in_dims[3];
	} else if (in_dims[3] == 3) {
		nhwc = true;
		ih   = in_dims[1];
		iw   = in_dims[2];
	} else {
		s.error = "model input has no channel-3 dim";
		return false;
	}
	if (ih <= 0 || iw <= 0) {
		LOG_MSG("OpenXR: Neural depth %s: dynamic spatial dims, using %lldx%lld",
		        def->id,
		        (long long)def->def_w,
		        (long long)def->def_h);
		ih = def->def_h;
		iw = def->def_w;
	}
	if (ih <= 0 || iw <= 0 || ih > 1024 || iw > 1024) {
		s.error = "bogus model input dims";
		return false;
	}
	s.in_h       = ih;
	s.in_w       = iw;
	s.input_nhwc = nhwc;
	s.in_elem_type = in_elem;
	s.in_shape_dims[0] = 1;
	if (nhwc) {
		s.in_shape_dims[1] = ih;
		s.in_shape_dims[2] = iw;
		s.in_shape_dims[3] = 3;
	} else {
		s.in_shape_dims[1] = 3;
		s.in_shape_dims[2] = ih;
		s.in_shape_dims[3] = iw;
	}
	// Output: float map; last two dims are the plane (rank 4/3/2 ok).
	char* out_name = nullptr;
	OrtTypeInfo* out_type = nullptr;
	ok = xr_ort_check(ort->SessionGetOutputName(s.session,
	                                            0,
	                                            s.alloc,
	                                            &out_name),
	                  "output name") &&
	     xr_ort_check(ort->SessionGetOutputTypeInfo(s.session,
	                                                0,
	                                                &out_type),
	                  "output type");
	if (ok && out_name) {
		s.output_name = out_name;
	}
	if (out_name) {
		ort->AllocatorFree(s.alloc, out_name);
	}
	const OrtTensorTypeAndShapeInfo* out_shape = nullptr;
	XrOrtTensorElementDataType out_elem = XR_ORT_ELEM_UNDEFINED;
	size_t out_rank = 0;
	int64_t out_dims[8] = {};
	ok = ok &&
	     xr_ort_check(ort->CastTypeInfoToTensorInfo(out_type, &out_shape),
	                  "output shape cast") &&
	     xr_ort_check(ort->GetTensorElementType(out_shape, &out_elem),
	                  "output element type") &&
	     xr_ort_check(ort->GetDimensionsCount(out_shape, &out_rank),
	                  "output rank") &&
	     (out_rank >= 2 && out_rank <= 4) &&
	     xr_ort_check(ort->GetDimensions(out_shape, out_dims, out_rank),
	                  "output dims");
	// Always log the discovered spec: decisive when a model refuses to
	// load (quantized I/O, dynamic dims, extra inputs). Only the
	// rank's own dims print (no zero padding past the rank).
	LOG_MSG("OpenXR: Neural depth %s spec: in '%s' rank %llu dims %s elem %s, out '%s' rank %llu dims %s elem %s",
	        def->id,
	        s.input_name.c_str(),
	        (unsigned long long)in_rank,
	        xr_ort_dims_str(in_dims, in_rank).c_str(),
	        xr_ort_elem_name(in_elem),
	        s.output_name.c_str(),
	        (unsigned long long)out_rank,
	        xr_ort_dims_str(out_dims, out_rank).c_str(),
	        xr_ort_elem_name(out_elem));
	// NOTE: TypeInfo/MemoryInfo/SessionOptions leak by design (a few
	// small objects per process; only Status and per-run Values are
	// released). Sessions/objects are process-lifetime: the worker is
	// joined at exit and the OS reclaims the rest.
	if (!ok || out_elem != XR_ORT_ELEM_FLOAT) {
		s.error = "unexpected model output (want float map)";
		return false;
	}
	s.out_h = out_dims[out_rank - 2];
	s.out_w = out_dims[out_rank - 1];
	if (s.out_h <= 0 || s.out_w <= 0) {
		// Dynamic output plane: dense models track the input size.
		LOG_MSG("OpenXR: Neural depth %s: dynamic output plane, using input %lldx%lld",
		        def->id,
		        (long long)iw,
		        (long long)ih);
		s.out_h = ih;
		s.out_w = iw;
	}
	if (s.out_h <= 0 || s.out_w <= 0 || s.out_h > 2048 ||
	    s.out_w > 2048) {
		s.error = "bogus model output dims";
		return false;
	}
	s.out_elem_type = out_elem;
	s.in_tensor.assign(static_cast<size_t>(3) * s.in_h * s.in_w, 0.0f);
	{
		std::lock_guard<std::mutex> lock(s.mtx);
		s.map.assign(static_cast<size_t>(s.out_h) * s.out_w, 0.0f);
		s.map_w = static_cast<int>(s.out_w);
		s.map_h = static_cast<int>(s.out_h);
		s.model_id      = def->id;
		s.invert_output = def->invert_output;
		s.imagenet_norm = def->imagenet_norm;
		// Fresh session: identical inputs must re-run.
		s.submit_hash_valid = false;
		s.tensor_hash_valid = false;
		s.ready             = true;
	}
	LOG_MSG("OpenXR: Neural depth ready (%s: in %s %lldx%lld %s %s, out %s %lldx%lld %s)",
	        def->id,
	        s.input_name.c_str(),
	        (long long)s.in_w,
	        (long long)s.in_h,
	        s.input_nhwc ? "NHWC" : "NCHW",
	        xr_ort_elem_name(s.in_elem_type),
	        s.output_name.c_str(),
	        (long long)s.out_w,
	        (long long)s.out_h,
	        xr_ort_elem_name(s.out_elem_type));
	return true;
}

// Area downscale: bottom-up ARGB32 to top-down RGB float. Plain [0,1]
// unless the model wants ImageNet normalization (float inputs only;
// quantized inputs always take raw bytes, normalized by their own
// scale below).
static void xr_neural_preprocess(const uint32_t* frame, int fw, int fh,
                                 float* dst, int dw, int dh,
                                 bool imagenet, bool nhwc)
{
	float mean[3]    = {0.0f, 0.0f, 0.0f};
	float inv_std[3] = {1.0f, 1.0f, 1.0f};
	if (imagenet) {
		mean[0]    = 0.485f;
		mean[1]    = 0.456f;
		mean[2]    = 0.406f;
		inv_std[0] = 1.0f / 0.229f;
		inv_std[1] = 1.0f / 0.224f;
		inv_std[2] = 1.0f / 0.225f;
	}
	for (int y = 0; y < dh; ++y) {
		// Model row 0 is the top: sample the frame bottom-up flipped.
		const float src_y = (y + 0.5f) * fh / dh;
		int y0 = static_cast<int>(fh - src_y - 0.5f);
		if (y0 < 0) {
			y0 = 0;
		}
		if (y0 >= fh) {
			y0 = fh - 1;
		}
		const auto* row = frame + static_cast<size_t>(y0) * fw;
		for (int x = 0; x < dw; ++x) {
			const float src_x = (x + 0.5f) * fw / dw;
			int x0 = static_cast<int>(src_x - 0.5f);
			if (x0 < 0) {
				x0 = 0;
			}
			if (x0 >= fw) {
				x0 = fw - 1;
			}
			const uint32_t p = row[x0];
			const float rgb[3] = {((p >> 16) & 0xFFu) / 255.0f,
			                      ((p >> 8) & 0xFFu) / 255.0f,
			                      (p & 0xFFu) / 255.0f};
			const size_t base = static_cast<size_t>(y) * dw + x;
			for (int c = 0; c < 3; ++c) {
				const float v = (rgb[c] - mean[c]) * inv_std[c];
				if (nhwc) {
					dst[base * 3 + c] = v;
				} else {
					dst[static_cast<size_t>(c) * dw * dh +
					    base] = v;
				}
			}
		}
	}
}

static void xr_neural_worker_main()
{
	auto& s = xr_neural;
	uint64_t run_count = 0;
	uint64_t run_fails = 0;
	for (;;) {
		// (Re)load when nothing is loaded or the toggle asked for a
		// different model; retry failures so files downloaded
		// mid-session are picked up with no restart.
		std::string want;
		{
			std::lock_guard<std::mutex> lock(s.mtx);
			want = s.request_model;
		}
		if (!s.ready || want != s.model_id) {
			if (!xr_neural_load_model(xr_neural_find(want))) {
				std::unique_lock<std::mutex> lock(s.mtx);
				s.load_done = true;
				if (!s.toast_done) {
					s.toast_done   = true;
					s.toast_needed = std::string("Neural depth: ") +
					                 s.error;
				}
				s.cv.wait_for(lock,
				              std::chrono::seconds(5),
				              [&] { return s.stop_requested; });
				if (s.stop_requested) {
					return;
				}
				continue;
			}
			{
				std::lock_guard<std::mutex> lock(s.mtx);
				s.load_done = true;
				s.cv.notify_all();
			}
		}
		auto* ort = s.ort;
		const char* in_names[1]  = {s.input_name.c_str()};
		const char* out_names[1] = {s.output_name.c_str()};
		std::vector<uint32_t> frame;
		int fw = 0, fh = 0;
		{
			std::unique_lock<std::mutex> lock(s.mtx);
			s.cv.wait(lock, [&] {
				return s.stop_requested || s.has_job;
			});
			if (s.stop_requested) {
				return;
			}
			frame.swap(s.job_frame);
			fw         = s.job_w;
			fh         = s.job_h;
			s.has_job  = false;
			s.job_busy = true;
		}
		if (fw <= 0 || fh <= 0 || frame.empty()) {
			std::lock_guard<std::mutex> lock(s.mtx);
			s.job_busy = false;
			continue;
		}
		xr_neural_preprocess(frame.data(),
		                     fw,
		                     fh,
		                     s.in_tensor.data(),
		                     static_cast<int>(s.in_w),
		                     static_cast<int>(s.in_h),
		                     s.imagenet_norm &&
		                             s.in_elem_type ==
		                                     XR_ORT_ELEM_FLOAT,
		                     s.input_nhwc);
		// Stage B: skip inference when the preprocessed tensor is
		// identical (downscaling can erase frame differences the
		// raw hash kept). CPU inference is deterministic, so the
		// published map stays correct.
		{
			const uint64_t th = xr_fnv1a(
			        s.in_tensor.data(),
			        s.in_tensor.size() * sizeof(float),
			        1469598103934665603ULL);
			std::lock_guard<std::mutex> lock(s.mtx);
			if (s.tensor_hash_valid && s.tensor_hash == th) {
				++s.inference_skipped;
				s.job_busy = false;
				continue;
			}
			s.tensor_hash       = th;
			s.tensor_hash_valid = true;
		}
		// Quantized inputs eat raw bytes scaled from the plain [0,1]
		// plane (ImageNet norm is float-only); float otherwise.
		void* in_data = s.in_tensor.data();
		size_t in_data_bytes = s.in_tensor.size() * sizeof(float);
		auto in_etype = XR_ORT_ELEM_FLOAT;
		if (s.in_elem_type == XR_ORT_ELEM_UINT8 ||
		    s.in_elem_type == XR_ORT_ELEM_INT8) {
			s.in_tensor_u8.assign(s.in_tensor.size(), 0);
			if (s.in_elem_type == XR_ORT_ELEM_UINT8) {
				for (size_t i = 0; i < s.in_tensor.size(); ++i) {
					float v = s.in_tensor[i] * 255.0f;
					if (v < 0.0f) {
						v = 0.0f;
					}
					if (v > 255.0f) {
						v = 255.0f;
					}
					s.in_tensor_u8[i] = static_cast<uint8_t>(v);
				}
			} else {
				for (size_t i = 0; i < s.in_tensor.size(); ++i) {
					int v = static_cast<int>(s.in_tensor[i] *
					                         255.0f) -
					        128;
					if (v < -128) {
						v = -128;
					}
					if (v > 127) {
						v = 127;
					}
					s.in_tensor_u8[i] =
					        static_cast<uint8_t>(v);
				}
			}
			in_data       = s.in_tensor_u8.data();
			in_data_bytes = s.in_tensor_u8.size();
			in_etype = static_cast<XrOrtTensorElementDataType>(
			        s.in_elem_type);
		}
		OrtValue* in_val  = nullptr;
		OrtValue* out_val = nullptr;
		std::string run_err;
		bool run_ok =
		        xr_ort_status_ok(ort->CreateTensorWithDataAsOrtValue(
		                             s.mem,
		                             in_data,
		                             in_data_bytes,
		                             s.in_shape_dims,
		                             4,
		                             in_etype,
		                             &in_val),
		                         run_err) &&
		        xr_ort_status_ok(ort->Run(s.session,
		                                  nullptr,
		                                  in_names,
		                                  const_cast<const OrtValue**>(
		                                          &in_val),
		                                  1,
		                                  out_names,
		                                  1,
		                                  &out_val),
		                         run_err);
		if (in_val) {
			ort->ReleaseValue(in_val);
		}
		if (!run_ok) {
			++run_fails;
			if (run_fails < 4 || run_fails % 60 == 0) {
				LOG_ERR("OpenXR: Neural depth inference failed "
				        "(#%llu): %s",
				        static_cast<unsigned long long>(run_fails),
				        run_err.c_str());
			}
		}
		if (run_ok && out_val) {
			void* out_data = nullptr;
			std::string data_err;
			if (xr_ort_status_ok(ort->GetTensorMutableData(out_val,
			                                              &out_data),
			                     data_err) &&
			    out_data) {
				const auto* f = static_cast<const float*>(out_data);
				float mn = std::numeric_limits<float>::max();
				float mx = std::numeric_limits<float>::lowest();
				double sum = 0.0;
				size_t n   = 0;
				{
					std::lock_guard<std::mutex> lock(s.mtx);
					n = std::min(s.map.size(),
					             static_cast<size_t>(s.out_h) *
					                     s.out_w);
					for (size_t i = 0; i < n; ++i) {
						const float v = f[i];
						s.map[i]      = v;
						if (v < mn) {
							mn = v;
						}
						if (v > mx) {
							mx = v;
						}
						sum += v;
					}
					++s.map_epoch;
				}
				++run_count;
				if (run_count % 60 == 0) {
					uint64_t skipped_submits = 0;
					uint64_t skipped_runs    = 0;
					{
						std::lock_guard<std::mutex> lock(
						        s.mtx);
						skipped_submits =
						        s.submit_skipped;
						skipped_runs =
						        s.inference_skipped;
					}
					LOG_MSG("OpenXR: Neural depth map #%llu (%s): "
					        "min %.4f max %.4f mean %.4f, "
					        "skipped %llu submits %llu runs",
					        static_cast<unsigned long long>(
					                run_count),
					        s.model_id.c_str(),
					        mn,
					        mx,
					        sum / std::max<size_t>(1, n),
					        static_cast<unsigned long long>(
					                skipped_submits),
					        static_cast<unsigned long long>(
					                skipped_runs));
				}
			}
			ort->ReleaseValue(out_val);
		}
		{
			std::lock_guard<std::mutex> lock(s.mtx);
			s.job_busy = false;
		}
	}
}

static void xr_neural_stop_at_exit()
{
	auto& s = xr_neural;
	{
		std::lock_guard<std::mutex> lock(s.mtx);
		s.stop_requested = true;
	}
	s.cv.notify_all();
	if (s.worker.joinable()) {
		s.worker.join();
	}
#if defined(_WIN32)
	if (s.lib) {
		FreeLibrary(s.lib);
		s.lib = nullptr;
	}
#elif defined(__linux__) || defined(__APPLE__)
	if (s.lib) {
		dlclose(s.lib);
		s.lib = nullptr;
	}
#endif
}

// Hand the latest frame to the worker (throttled); starts the worker
// (and its async model load) on first neural use. Never blocks.
void xr_neural_submit_frame(const uint32_t* frame, int w, int h)
{
	auto& s = xr_neural;
	if (!s.worker_running) {
		s.worker_running = true;
		{
			// Default model for this session; the toggle can switch
			// it live afterwards.
			std::lock_guard<std::mutex> lock(s.mtx);
			if (s.request_model.empty()) {
				const std::string def_id =
				        get_xr_section()->GetString(
				                "xr_neural_model");
				s.request_model = xr_neural_find(def_id)->id;
			}
		}
		std::call_once(xr_neural_stop_flag, [] {
			std::atexit(xr_neural_stop_at_exit);
		});
		try {
			s.worker = std::thread(xr_neural_worker_main);
		} catch (const std::exception& e) {
			LOG_ERR("OpenXR: Neural depth thread failed: %s", e.what());
			s.worker_running = false;
			return;
		}
		if (!s.toast_loading) {
			s.toast_loading = true;
			OPENXR_Notify("Neural depth: loading model...");
		}
	}
	int interval_ms = get_xr_section()->GetInt("xr_neural_interval_ms");
	if (interval_ms < 16) {
		interval_ms = 16;
	}
	if (interval_ms > 5000) {
		interval_ms = 5000;
	}
	const int64_t now_us = GetTicksUs();
	if (!frame || w <= 0 || h <= 0) {
		return;
	}
	// Stage A: skip identical frames before copying anything (~4k
	// pixel samples). Static DOS content then costs ~nothing; the
	// timer only advances on real submits so a change is picked up
	// immediately.
	const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
	const size_t stride = (pixels > 4096) ? pixels / 4096 : 1;
	uint64_t fh        = 1469598103934665603ULL;
	fh                 = xr_fnv1a(&w, sizeof(w), fh);
	fh                 = xr_fnv1a(&h, sizeof(h), fh);
	for (size_t i = 0; i < pixels; i += stride) {
		fh = xr_fnv1a(&frame[i], sizeof(frame[i]), fh);
	}
	{
		std::lock_guard<std::mutex> lock(s.mtx);
		// Don't pile up: the worker always has the latest queued
		// frame waiting when it is free.
		if (s.has_job || s.job_busy) {
			return;
		}
		if (s.submit_hash_valid && s.submit_hash == fh) {
			++s.submit_skipped;
			return;
		}
		if (s.ready && now_us - s.last_submit_us <
		                static_cast<int64_t>(interval_ms) * 1000) {
			return;
		}
		s.last_submit_us   = now_us;
		s.submit_hash      = fh;
		s.submit_hash_valid = true;
		s.job_frame.assign(frame, frame + pixels);
		s.job_w   = w;
		s.job_h   = h;
		s.has_job = true;
	}
	s.cv.notify_one();
}

// Main-thread side of the worker's warnings (the worker must never
// touch the OSD queue itself).
void xr_neural_poll_missing_toast()
{
	auto& s = xr_neural;
	std::string needed;
	{
		std::lock_guard<std::mutex> lock(s.mtx);
		if (!s.toast_needed.empty()) {
			needed = s.toast_needed;
			s.toast_needed.clear();
		}
	}
	if (!needed.empty()) {
		OPENXR_Notify(needed);
	}
}

// Upsample the published model map (nearest) into a frame-sized,
// normalized depth map (1 = near). Returns false when no map is ready
// (caller falls back to the heuristic cues); flat when spread is nil.
bool xr_neural_fill_map(float* depth_norm, int w, int h, bool& flat)
{
	auto& s = xr_neural;
	std::lock_guard<std::mutex> lock(s.mtx);
	if (!s.ready || s.map_epoch == 0 || s.map_w <= 0 || s.map_h <= 0 ||
	    s.map.size() != static_cast<size_t>(s.map_w) * s.map_h) {
		return false;
	}
	const bool invert = (s.invert_output !=
	                     get_xr_section()->GetBool("xr_autodepth_invert"));
	float dmin = std::numeric_limits<float>::max();
	float dmax = std::numeric_limits<float>::lowest();
	for (int y = 0; y < h; ++y) {
		// Model row 0 is the top; frame rows run bottom-up.
		const int my = (h - 1 - y) * s.map_h / h;
		const auto* mrow = s.map.data() + static_cast<size_t>(my) *
		                                        s.map_w;
		auto* drow = depth_norm + static_cast<size_t>(y) * w;
		for (int x = 0; x < w; ++x) {
			const int mx = x * s.map_w / w;
			const float v = mrow[mx];
			drow[x]      = v;
			if (v < dmin) {
				dmin = v;
			}
			if (v > dmax) {
				dmax = v;
			}
		}
	}
	if (dmax <= dmin) {
		for (int i = 0; i < w * h; ++i) {
			depth_norm[i] = 0.0f;
		}
		flat = true;
		return true;
	}
	const float inv_range = 1.0f / (dmax - dmin);
	for (int i = 0; i < w * h; ++i) {
		// MiDaS outputs inverse depth: near reads high already.
		const float d = (depth_norm[i] - dmin) * inv_range;
		depth_norm[i] = invert ? (1.0f - d) : d;
	}
	flat = false;
	if (!s.toast_active) {
		s.toast_active = true;
		OPENXR_Notify("Neural depth active");
	}
	return true;
}
#endif // C_NEURAL

#if C_NEURAL
// Cycle the downloaded neural models in the given direction (+1 next,
// -1 previous). Shared by the Ctrl+N shortcut and the VR menu list.
void xr_neural_cycle_model(const int dir)
{
	// Cycle the downloaded models only.
	std::vector<int> present;
	for (int i = 0; i < xr_neural_model_count; ++i) {
		std::error_code ec;
		const auto p    = xr_neural_dir() / xr_neural_models[i].file;
		const auto size = std_fs::file_size(p, ec);
		if (!ec && size > 1024 * 1024) {
			present.push_back(i);
		}
	}
	if (present.empty()) {
		OPENXR_Notify("No neural models downloaded: run XRDEPTH DOWNLOAD");
		return;
	}
	auto& s = xr_neural;
	{
		std::lock_guard<std::mutex> lock(s.mtx);
		int at = 0;
		for (size_t k = 0; k < present.size(); ++k) {
			if (xr_neural_models[present[k]].id == s.request_model) {
				at = static_cast<int>(k);
				break;
			}
		}
		const int n = static_cast<int>(present.size());
		at = (at + dir % n + n) % n;
		s.request_model =
		        xr_neural_models[present[at]].id;
	}
	s.cv.notify_one();
	OPENXR_Notify(format_str("Neural model: %s", s.request_model.c_str()));
}

// Wanted model id for the VR menu (locked read; empty = none yet).
std::string openxr_menu_neural_model()
{
	auto& s = xr_neural;
	std::lock_guard<std::mutex> lock(s.mtx);
	return s.request_model;
}
#endif // C_NEURAL

#endif // C_OPENXR
