// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "xrdepth.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "misc/cross.h"
#include "misc/std_filesystem.h"
#include "shell/shell.h"
#include "utils/checks.h"
#include "utils/string_utils.h"

CHECK_NARROWING();

// Neural-depth file set for the 'neural' autodepth submode. Mirrors the
// loader in gui/sdl_gui.cpp (keep filenames in sync):
//   <config>/xrdepth/midas_v21_small_256.onnx  (~66 MB MiDaS v2.1 small)
//   <config>/xrdepth/onnxruntime.dll           (plus providers_shared, if any)
static constexpr const char* XrDepthDirName = "xrdepth";
static constexpr const char* XrDepthModelFile = "midas_v21_small_256.onnx";
static constexpr const char* XrDepthOrtDll = "onnxruntime.dll";
static constexpr const char* XrDepthProvidersDll =
        "onnxruntime_providers_shared.dll";
// Pinned sources (verified sizes in brackets).
static constexpr const char* XrDepthModelUrl =
        "https://huggingface.co/julienkay/sentis-MiDaS/resolve/main/onnx/"
        "midas_v21_small_256.onnx"; // ~66 MB
static constexpr const char* XrDepthOrtZipUrl =
        "https://github.com/microsoft/onnxruntime/releases/download/"
        "v1.30.0/onnxruntime-win-x64-1.30.0.zip"; // ~83 MB
static constexpr int64_t XrDepthModelMinBytes = 50 * 1024 * 1024;
static constexpr int64_t XrDepthOrtZipMinBytes = 70 * 1024 * 1024;
static constexpr int64_t XrDepthDllMinBytes = 5 * 1024 * 1024;

static std_fs::path xrdepth_dir()
{
	return get_config_dir() / XrDepthDirName;
}

static void xrdepth_status(Program& prog)
{
	const auto dir = xrdepth_dir();
	prog.WriteOut("Neural depth files in %s:\n", dir.string().c_str());
	bool ok = true;
	for (const char* name : {XrDepthModelFile, XrDepthOrtDll}) {		const auto p    = dir / name;
		std::error_code ec;
		const auto size = std_fs::file_size(p, ec);
		const int64_t min =
		        (std::strcmp(name, XrDepthModelFile) == 0)
		        ? XrDepthModelMinBytes
		        : XrDepthDllMinBytes;
		if (!ec &&
		    size >= static_cast<std::uintmax_t>(min)) {
			prog.WriteOut("  %-32s %6.1f MB ok\n",
			              name,
			              size / 1048576.0);
		} else {
			prog.WriteOut("  %-32s missing\n", name);
			ok = false;
		}
	}
	{
		// Optional companion lib (shipped in some runtime zips).
		const auto p    = dir / XrDepthProvidersDll;
		std::error_code ec;
		const auto size = std_fs::file_size(p, ec);
		if (!ec && size >= static_cast<std::uintmax_t>(XrDepthDllMinBytes)) {
			prog.WriteOut("  %-32s %6.1f MB ok\n",
			              XrDepthProvidersDll,
			              size / 1048576.0);
		}
	}
	if (ok) {
		prog.WriteOut("Ready: set xr_autodepth_method to neural (Ctrl+A\n"
		              "cycles it). No restart needed.\n");
	} else {
		prog.WriteOut("Run XRDEPTH DOWNLOAD to fetch the missing files.\n");
	}
}

#if defined(_WIN32)
#include <windows.h>
// objbase/urlmon before use: IUnknown, IBindStatusCallback, BINDINFO.
#include <objbase.h>
#include <urlmon.h>

// Minimal IBindStatusCallback: progress only, everything else default.
class XrDepthBindStatus : public IBindStatusCallback {
public:
	explicit XrDepthBindStatus(Program& prog) : prog(prog) {}
	virtual ~XrDepthBindStatus() = default;

	STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override
	{
		if (!ppv) {
			return E_POINTER;
		}
		if (riid == IID_IUnknown ||
		    riid == IID_IBindStatusCallback) {
			*ppv = static_cast<IBindStatusCallback*>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHOD_(ULONG, AddRef)() override { return ++refs; }
	STDMETHOD_(ULONG, Release)() override
	{
		const auto r = --refs;
		if (r == 0) {
			delete this;
		}
		return r;
	}
	STDMETHOD(GetBindInfo)(DWORD*, BINDINFO*) override
	{
		return E_NOTIMPL;
	}
	STDMETHOD(OnStartBinding)(DWORD, IBinding*) override { return S_OK; }
	STDMETHOD(GetPriority)(LONG*) override { return E_NOTIMPL; }
	STDMETHOD(OnLowResource)(DWORD) override { return S_OK; }
	STDMETHOD(OnProgress)(ULONG done, ULONG total, ULONG code,
	                       LPCWSTR) override
	{
		if (code == BINDSTATUS_DOWNLOADINGDATA && total > 0) {
			const int pct = static_cast<int>(done * 100 / total);
			if (pct >= next_pct) {
				next_pct = pct + 10;
				prog.WriteOut("  ... %d%%\n", pct);
			}
		}
		return S_OK;
	}
	STDMETHOD(OnStopBinding)(HRESULT, LPCWSTR) override { return S_OK; }
	STDMETHOD(OnDataAvailable)(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override
	{
		return S_OK;
	}
	STDMETHOD(OnObjectAvailable)(REFIID, IUnknown*) override
	{
		return S_OK;
	}

private:
	Program& prog;
	ULONG refs     = 1;
	int next_pct = 0;
};

using UrlDownloadFn = HRESULT(WINAPI*)(LPUNKNOWN,
                                       LPCWSTR,
                                       LPCWSTR,
                                       DWORD,
                                       LPBINDSTATUSCALLBACK);
using CoInitFn   = HRESULT(WINAPI*)(LPVOID, DWORD);
using CoUninitFn = void(WINAPI*)();

static bool xrdepth_download_file(Program& prog, const char* url,
                                  const std_fs::path& dest,
                                  const char* label)
{
	HMODULE urlmon = LoadLibraryW(L"urlmon.dll");
	if (!urlmon) {
		prog.WriteOut("Cannot load urlmon.dll.\n");
		return false;
	}
	// Via void* to avoid function-type-mismatch warnings; the signatures
	// below mirror urlmon.h/ole32 (verified) for these stdcall entry points.
	auto* url_download = reinterpret_cast<UrlDownloadFn>(
	        reinterpret_cast<void*>(
	                GetProcAddress(urlmon, "URLDownloadToFileW")));
	if (!url_download) {
		FreeLibrary(urlmon);
		prog.WriteOut("URLDownloadToFileW not found.\n");
		return false;
	}
	HMODULE ole32 = LoadLibraryW(L"ole32.dll");
	CoInitFn co_init     = nullptr;
	CoUninitFn co_uninit = nullptr;
	bool com_ready       = false;
	bool com_ours        = false;
	if (ole32) {
		co_init = reinterpret_cast<CoInitFn>(reinterpret_cast<void*>(
		        GetProcAddress(ole32, "CoInitializeEx")));
		co_uninit = reinterpret_cast<CoUninitFn>(
		        reinterpret_cast<void*>(
		                GetProcAddress(ole32, "CoUninitialize")));
		if (co_init) {
			const HRESULT hr = co_init(
			        nullptr, COINIT_APARTMENTTHREADED);
			com_ready = (hr == S_OK || hr == S_FALSE);
			com_ours  = (hr == S_OK);
		}
	}
	prog.WriteOut("Downloading %s ...\n", label);
	std::wstring wurl(url, url + strlen(url));
	auto* cb = new XrDepthBindStatus(prog);
	const HRESULT hr =
	        url_download(nullptr, wurl.c_str(), dest.wstring().c_str(),
	                     0, cb);
	cb->Release();
	if (com_ours && co_uninit) {
		co_uninit();
	}
	if (ole32) {
		FreeLibrary(ole32);
	}
	FreeLibrary(urlmon);
	if (FAILED(hr) || !com_ready) {
		prog.WriteOut("Download failed (0x%08lx).\n",
		              static_cast<unsigned long>(hr));
		return false;
	}
	return true;
}

static bool xrdepth_run_tar(Program& prog, const std_fs::path& zip,
                            const std_fs::path& dest_dir)
{
	std::wstring cmd = L"tar.exe -xf \"";
	cmd += zip.wstring();
	cmd += L"\" -C \"";
	cmd += dest_dir.wstring();
	cmd += L"\"";
	STARTUPINFOW si = {};
	si.cb           = sizeof(si);
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW(nullptr,
	                    cmd.data(),
	                    nullptr,
	                    nullptr,
	                    FALSE,
	                    CREATE_NO_WINDOW,
	                    nullptr,
	                    nullptr,
	                    &si,
	                    &pi)) {
		prog.WriteOut("Cannot start tar.exe (needs Windows 10+).\n");
		return false;
	}
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	if (code != 0) {
		prog.WriteOut("tar.exe failed (%lu).\n",
		              static_cast<unsigned long>(code));
		return false;
	}
	return true;
}

static void xrdepth_download_all(Program& prog)
{
	const auto dir = xrdepth_dir();
	std::error_code ec;
	std_fs::create_directories(dir, ec);
	if (ec) {
		prog.WriteOut("Cannot create %s.\n", dir.string().c_str());
		return;
	}
	auto size_of = [](const std_fs::path& p) -> int64_t {
		std::error_code ec2;
		const auto s = std_fs::file_size(p, ec2);
		return ec2 ? -1 : static_cast<int64_t>(s);
	};
	// 1. ONNX Runtime zip (skip when the DLL is already valid).
	bool have_ort = size_of(dir / XrDepthOrtDll) >= XrDepthDllMinBytes;
	const auto zip_path = dir / "onnxruntime-win-x64.zip";
	if (!have_ort) {
		if (!xrdepth_download_file(prog,
		                           XrDepthOrtZipUrl,
		                           zip_path,
		                           "ONNX Runtime (~83 MB)")) {
			return;
		}
		if (size_of(zip_path) < XrDepthOrtZipMinBytes) {
			prog.WriteOut("Archive looks truncated, aborting.\n");
			std_fs::remove(zip_path, ec);
			return;
		}
		const auto stage = dir / "ort_stage";
		std_fs::remove_all(stage, ec);
		std_fs::create_directories(stage, ec);
		if (!xrdepth_run_tar(prog, zip_path, stage)) {
			return;
		}
		// Move every shipped DLL up (onnxruntime.dll is required,
		// *_providers_shared.dll rides along when present).
		bool moved_ort = false;
		for (const auto& entry :
		     std_fs::recursive_directory_iterator(stage, ec)) {
			if (!entry.is_regular_file(ec)) {
				continue;
			}
			const auto& src = entry.path();
			if (src.extension() != ".dll") {
				continue;
			}
			std_fs::rename(src, dir / src.filename(), ec);
			if (!ec && src.filename() == XrDepthOrtDll) {
				moved_ort = true;
			}
		}
		std_fs::remove_all(stage, ec);
		std_fs::remove(zip_path, ec);
		if (!moved_ort ||
		    size_of(dir / XrDepthOrtDll) < XrDepthDllMinBytes) {
			prog.WriteOut("onnxruntime.dll missing from archive.\n");
			return;
		}
		prog.WriteOut("ONNX Runtime installed.\n");
	} else {
		prog.WriteOut("ONNX Runtime already present, skipping.\n");
	}
	// 2. Depth model (skip when already valid).
	if (size_of(dir / XrDepthModelFile) >= XrDepthModelMinBytes) {
		prog.WriteOut("Depth model already present, skipping.\n");
	} else {
		if (!xrdepth_download_file(prog,
		                           XrDepthModelUrl,
		                           dir / XrDepthModelFile,
		                           "MiDaS small model (~66 MB)")) {
			return;
		}
		if (size_of(dir / XrDepthModelFile) < XrDepthModelMinBytes) {
			prog.WriteOut("Model file looks truncated, aborting.\n");
			std_fs::remove(dir / XrDepthModelFile, ec);
			return;
		}
		prog.WriteOut("Depth model installed.\n");
	}
	prog.WriteOut("Done. Switch xr_autodepth_method to neural (Ctrl+A\n"
	              "cycles it); no restart needed.\n");
}
#endif // _WIN32

void XRDEPTH::Run()
{
	if (HelpRequested()) {
		WriteOut("Manages the neural-depth files for the 'neural'\n"
		         "autodepth submode (ONNX Runtime + MiDaS small model).\n"
		         "\n"
		         "Usage:\n"
		         "  XRDEPTH            Show download status.\n"
		         "  XRDEPTH DOWNLOAD   Fetch the missing files into the\n"
		         "                     DOSBox config folder (no restart\n"
		         "                     needed afterwards).\n");
		return;
	}
	std::string arg;
	if (cmd->FindString("DOWNLOAD", arg, false) ||
	    cmd->FindString("download", arg, false)) {
#if defined(_WIN32)
		xrdepth_download_all(*this);
#else
		WriteOut("Automatic download is Windows-only. Fetch manually:\n"
		         "  %s\n"
		         "  %s\n"
		         "and place midas_v21_small_256.onnx plus the matching\n"
		         "onnxruntime library in <config>/xrdepth/.\n",
		         XrDepthModelUrl,
		         XrDepthOrtZipUrl);
#endif
		return;
	}
	xrdepth_status(*this);
}
