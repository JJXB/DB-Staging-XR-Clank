// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gui/render/lsfg.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "config/setup.h"
#include "misc/cross.h"
#include "misc/logging.h"

namespace {

// RT_RCDATA resource type id.
constexpr uint32_t ResTypeRcdata = 10;

// SPIR-V magic (file bytes 03 02 23 07, little-endian word).
constexpr uint32_t MagicSpirv = 0x07230203u;

// "DXBC" as a little-endian word.
constexpr uint32_t MagicDxbc = 0x43425844u;

// FNV-1a 64-bit over a byte range.
uint64_t fnv1a64(const uint8_t* data, size_t len)
{
	uint64_t h = 14695981039346656037ull;
	for (size_t i = 0; i < len; ++i) {
		h ^= data[i];
		h *= 1099511628211ull;
	}
	return h;
}

std::string to_hex64(uint64_t v)
{
	char buf[17] = {};
	snprintf(buf, sizeof(buf), "%016llx",
	         static_cast<unsigned long long>(v));
	return buf;
}

// Bounds-checked little-endian PE reader.
class PeView {
public:
	PeView(const uint8_t* data, size_t len) : d(data), n(len) {}

	bool u16(size_t off, uint16_t& v) const
	{
		if (off + 2 > n) {
			return false;
		}
		v = static_cast<uint16_t>(d[off] | (d[off + 1] << 8));
		return true;
	}

	bool u32(size_t off, uint32_t& v) const
	{
		if (off + 4 > n) {
			return false;
		}
		v = static_cast<uint32_t>(d[off] | (d[off + 1] << 8) |
		                          (d[off + 2] << 16) |
		                          (d[off + 3] << 24));
		return true;
	}

	bool in_range(size_t off, size_t len) const
	{
		return len <= n && off <= n - len;
	}

private:
	const uint8_t* d;
	size_t n;
};

struct PeSection {
	uint32_t vaddr   = 0;
	uint32_t vsize   = 0;
	uint32_t raw_ptr = 0;
	uint32_t raw_len = 0;
};

// Map an RVA to a file offset via the section table.
bool rva_to_raw(const std::vector<PeSection>& sections, uint32_t rva,
                uint32_t want_len, size_t& raw_out)
{
	for (const auto& s : sections) {
		const uint32_t span = s.vsize > s.raw_len ? s.vsize : s.raw_len;
		if (rva < s.vaddr || want_len > span) {
			continue;
		}
		const uint32_t rel = rva - s.vaddr;
		if (rel + want_len <= span && rel + want_len <= s.raw_len) {
			raw_out = static_cast<size_t>(s.raw_ptr + rel);
			return true;
		}
	}
	return false;
}

struct RcBlob {
	int id            = -1; // numeric resource id, -1 when named
	uint32_t size     = 0;
	uint32_t data_rva = 0;
};

// Read the entry counts of a resource directory header.
bool res_dir_counts(const PeView& pe, size_t dir_raw, uint32_t& total_out)
{
	uint16_t named = 0, ids = 0;
	if (!pe.u16(dir_raw + 12, named) || !pe.u16(dir_raw + 14, ids)) {
		return false;
	}
	total_out = static_cast<uint32_t>(named) + ids;
	return true;
}

// Collect RT_RCDATA blobs: root(type) -> name/id -> language -> data.
// Subdirectory offsets are relative to the resource section base.
// Directory entries are 8 bytes (name/id + target); headers are 16.
bool collect_rcdata(const PeView& pe, size_t root_raw,
                    std::vector<RcBlob>& out, std::string& error)
{
	uint32_t root_total = 0;
	if (!pe.in_range(root_raw, 16) ||
	    !res_dir_counts(pe, root_raw, root_total)) {
		error = "resource root out of range";
		return false;
	}
	for (uint32_t i = 0; i < root_total; ++i) {
		const size_t e_off = root_raw + 16 + static_cast<size_t>(i) * 8;
		uint32_t type_field = 0, target = 0;
		if (!pe.u32(e_off, type_field) || !pe.u32(e_off + 4, target)) {
			error = "resource root entry out of range";
			return false;
		}
		if ((target & 0x80000000u) == 0) {
			continue; // type level must be a subdirectory
		}
		if ((type_field & 0x80000000u) != 0 ||
		    type_field != ResTypeRcdata) {
			continue; // only RT_RCDATA (id 10)
		}
		const size_t l2_raw = root_raw + (target & 0x7fffffffu);
		uint32_t l2_total = 0;
		if (!pe.in_range(l2_raw, 16) ||
		    !res_dir_counts(pe, l2_raw, l2_total)) {
			error = "RCDATA name level out of range";
			return false;
		}
		for (uint32_t j = 0; j < l2_total; ++j) {
			const size_t f_off = l2_raw + 16 +
			                     static_cast<size_t>(j) * 8;
			uint32_t name_field = 0, lang_target = 0;
			if (!pe.u32(f_off, name_field) ||
			    !pe.u32(f_off + 4, lang_target)) {
				error = "RCDATA name entry out of range";
				return false;
			}
			if ((lang_target & 0x80000000u) == 0) {
				continue; // expect a language subdirectory
			}
			const size_t l3_raw = root_raw +
			                      (lang_target & 0x7fffffffu);
			uint32_t l3_total = 0;
			if (!pe.in_range(l3_raw, 16) ||
			    !res_dir_counts(pe, l3_raw, l3_total) ||
			    l3_total == 0) {
				error = "RCDATA language level out of range";
				return false;
			}
			// First language entry wins (all LANG_ENGLISH here):
			// +16 is the language id, +20 is the data-entry offset.
			uint32_t data_entry_off = 0;
			if (!pe.u32(l3_raw + 20, data_entry_off) ||
			    (data_entry_off & 0x80000000u) != 0) {
				error = "RCDATA language entry out of range";
				return false;
			}
			const size_t de_raw = root_raw +
			                      (data_entry_off & 0x7fffffffu);
			uint32_t data_rva = 0, data_size = 0;
			if (!pe.in_range(de_raw, 16) ||
			    !pe.u32(de_raw, data_rva) ||
			    !pe.u32(de_raw + 4, data_size)) {
				error = "RCDATA data entry out of range";
				return false;
			}
			RcBlob b;
			b.id = ((name_field & 0x80000000u) != 0)
			        ? -1
			        : static_cast<int>(name_field);
			b.size     = data_size;
			b.data_rva = data_rva;
			out.push_back(b);
		}
		return true; // type 10 found and walked
	}
	error = "no RT_RCDATA type in resource root";
	return false;
}

// Parse headers, map sections, collect RCDATA entries.
bool parse_pe_rcdata(const std::vector<uint8_t>& img,
                     std::vector<PeSection>& sections_out,
                     std::vector<RcBlob>& blobs_out, std::string& error)
{
	const PeView pe(img.data(), img.size());
	uint32_t e_lfanew = 0;
	if (img.size() < 0x40 || img[0] != 'M' || img[1] != 'Z' ||
	    !pe.u32(0x3c, e_lfanew)) {
		error = "not a PE file (MZ missing)";
		return false;
	}
	uint32_t sig = 0;
	uint16_t num_sections = 0, opt_size = 0;
	if (!pe.u32(e_lfanew, sig) || sig != 0x00004550u ||
	    !pe.u16(e_lfanew + 6, num_sections) ||
	    !pe.u16(e_lfanew + 20, opt_size)) {
		error = "not a PE file (NT header missing)";
		return false;
	}
	const size_t opt_off     = e_lfanew + 24;
	const size_t sectab_off  = opt_off + opt_size;
	uint16_t opt_magic       = 0;
	if (!pe.u16(opt_off, opt_magic) ||
	    (opt_magic != 0x10bu && opt_magic != 0x20bu)) {
		error = "unsupported optional header magic";
		return false;
	}
	// Data directories start 96 (PE32) or 112 (PE32+) into the optional
	// header; the resource table is index 2.
	const size_t dd_off = opt_off + (opt_magic == 0x10bu ? 96 : 112);
	uint32_t rsrc_rva = 0, rsrc_size = 0;
	if (!pe.u32(dd_off + 2 * 8, rsrc_rva) ||
	    !pe.u32(dd_off + 2 * 8 + 4, rsrc_size) || rsrc_size == 0) {
		error = "no resource table";
		return false;
	}
	for (uint16_t s = 0; s < num_sections; ++s) {
		const size_t sh_off = sectab_off + static_cast<size_t>(s) * 40;
		uint32_t vsize = 0, vaddr = 0, raw_len = 0, raw_ptr = 0;
		if (!pe.in_range(sh_off, 40) || !pe.u32(sh_off + 8, vsize) ||
		    !pe.u32(sh_off + 12, vaddr) || !pe.u32(sh_off + 16, raw_len) ||
		    !pe.u32(sh_off + 20, raw_ptr)) {
			error = "section table out of range";
			return false;
		}
		sections_out.push_back({vaddr, vsize, raw_ptr, raw_len});
	}
	size_t root_raw = 0;
	if (!rva_to_raw(sections_out, rsrc_rva, 16, root_raw)) {
		error = "resource section unmappable";
		return false;
	}
	return collect_rcdata(pe, root_raw, blobs_out, error);
}

bool write_file(const std::filesystem::path& p,
                const uint8_t* data, size_t len)
{
	std::ofstream f(p, std::ios::binary | std::ios::trunc);
	if (!f) {
		return false;
	}
	if (len) {
		f.write(reinterpret_cast<const char*>(data),
		        static_cast<std::streamsize>(len));
	}
	return static_cast<bool>(f);
}

bool manifest_matches(const std::filesystem::path& manifest_path,
                      size_t dll_size, const std::string& fingerprint)
{
	std::ifstream f(manifest_path);
	if (!f) {
		return false;
	}
	bool size_ok = false, hash_ok = false;
	std::string line;
	while (std::getline(f, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line == "dll_size=" + std::to_string(dll_size)) {
			size_ok = true;
		}
		if (line == "dll_fnv1a64=" + fingerprint) {
			hash_ok = true;
		}
	}
	return size_ok && hash_ok;
}

// --- evaluation state -----------------------------------------------------

enum class LsfgState {
	Unchecked,
	Disabled, // method != lsfg, or no DLL configured
	Error,
	ReadyCached,
	ReadyFresh,
};

struct LsfgCache {
	bool evaluated      = false;
	std::string method_seen;
	std::string dll_seen;
	LsfgState state     = LsfgState::Unchecked;
	std::string dll_path;
	std::string fingerprint;
	size_t blob_count   = 0;
	size_t dxbc_count   = 0;
	size_t spirv_count  = 0;
	std::string error;
};

LsfgCache& cache()
{
	static LsfgCache c;
	return c;
}

std::string current_dll_path(SectionProp* section)
{
	if (!section) {
		return {};
	}
	const auto prop = section->GetPath("framegen_lsfg_dll");
	if (prop) {
		const auto p = prop->realpath.string();
		if (!p.empty()) {
			return p;
		}
	}
	return section->GetString("framegen_lsfg_dll");
}

void do_extract(const std::string& dll_path)
{
	auto& c    = cache();
	c.dll_path = dll_path;

	std::ifstream f(dll_path, std::ios::binary | std::ios::ate);
	if (!f) {
		c.state = LsfgState::Error;
		c.error = "cannot open file (check framegen_lsfg_dll)";
		LOG_ERR("LSFG: Cannot open '%s'; set [framegen] framegen_lsfg_dll "
		        "to your Lossless Scaling Lossless.dll",
		        dll_path.c_str());
		return;
	}
	const auto fsize = static_cast<size_t>(f.tellg());
	f.seekg(0);
	std::vector<uint8_t> img(fsize);
	if (fsize && !f.read(reinterpret_cast<char*>(img.data()),
	                     static_cast<std::streamsize>(fsize))) {
		c.state = LsfgState::Error;
		c.error = "cannot read file";
		LOG_ERR("LSFG: Cannot read '%s'", dll_path.c_str());
		return;
	}

	const std::string fingerprint = to_hex64(fnv1a64(img.data(), img.size()));
	c.fingerprint                 = fingerprint;
	const auto dir           = get_config_dir() / "lsfg" / fingerprint;
	const auto manifest_path = dir / "manifest.txt";

	if (manifest_matches(manifest_path, img.size(), fingerprint)) {
		// Cache hit: re-read counts from the manifest for status.
		c.blob_count = c.dxbc_count = c.spirv_count = 0;
		std::ifstream m(manifest_path);
		std::string line;
		while (std::getline(m, line)) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			if (line.rfind("blob ", 0) == 0) {
				++c.blob_count;
				if (line.find(" DXBC") != std::string::npos) {
					++c.dxbc_count;
				} else if (line.find(" SPIRV") != std::string::npos) {
					++c.spirv_count;
				}
			}
		}
		c.state = LsfgState::ReadyCached;
		LOG_MSG("LSFG: Shader cache hit (%s): %zu blobs "
		        "(%zu DXBC + %zu SPIR-V); DLL untouched",
		        fingerprint.c_str(), c.blob_count, c.dxbc_count,
		        c.spirv_count);
		return;
	}

	// Cache miss (first use, or the DLL changed): parse + extract.
	std::vector<PeSection> sections;
	std::vector<RcBlob> blobs;
	std::string error;
	if (!parse_pe_rcdata(img, sections, blobs, error)) {
		c.state = LsfgState::Error;
		c.error = error.empty() ? "PE parse failed" : error;
		LOG_ERR("LSFG: Cannot use '%s' (%s)", dll_path.c_str(),
		        c.error.c_str());
		return;
	}

	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	if (ec) {
		c.state = LsfgState::Error;
		c.error = "cannot create cache dir " + dir.string();
		LOG_ERR("LSFG: %s", c.error.c_str());
		return;
	}

	size_t dxbc = 0, spirv = 0, written = 0;
	std::string manifest =
	        "# DOSBox-XR LSFG shader cache: extracted once from the user's "
	        "own Lossless.dll copy (parsed as data, never executed).\n"
	        "# Not redistributable; re-extracted automatically if the DLL "
	        "changes.\n";
	manifest += "dll_path=" + dll_path + "\n";
	manifest += "dll_size=" + std::to_string(img.size()) + "\n";
	manifest += "dll_fnv1a64=" + fingerprint + "\n";
	for (size_t i = 0; i < blobs.size(); ++i) {
		const auto& b = blobs[i];
		size_t raw    = 0;
		if (b.size == 0 ||
		    !rva_to_raw(sections, b.data_rva, b.size, raw) ||
		    raw + b.size > img.size()) {
			c.state = LsfgState::Error;
			c.error = "RCDATA payload out of range";
			LOG_ERR("LSFG: Cannot use '%s' (%s)", dll_path.c_str(),
			        c.error.c_str());
			return;
		}
		char name[64] = {};
		if (b.id >= 0) {
			snprintf(name, sizeof(name), "rcdata_%d.bin", b.id);
		} else {
			snprintf(name, sizeof(name), "rcdata_named_%zu.bin", i);
		}
		if (!write_file(dir / name, img.data() + raw, b.size)) {
			c.state = LsfgState::Error;
			c.error = "cannot write " + (dir / name).string();
			LOG_ERR("LSFG: %s", c.error.c_str());
			return;
		}
		uint32_t magic = 0;
		if (b.size >= 4) {
			magic = static_cast<uint32_t>(img[raw] |
			                              (img[raw + 1] << 8) |
			                              (img[raw + 2] << 16) |
			                              (img[raw + 3] << 24));
		}
		std::string kind = "OTHER";
		if (magic == MagicDxbc) {
			kind = "DXBC";
			++dxbc;
		} else if (magic == MagicSpirv) {
			kind = "SPIRV";
			++spirv;
		}
		char row[128] = {};
		snprintf(row, sizeof(row), "blob %d %u %s\n", b.id,
		         static_cast<unsigned>(b.size), kind.c_str());
		manifest += row;
		++written;
	}
	manifest += "resource_count=" + std::to_string(written) + "\n";
	manifest += "dxbc_count=" + std::to_string(dxbc) + "\n";
	manifest += "spirv_count=" + std::to_string(spirv) + "\n";
	{
		std::ofstream m(manifest_path, std::ios::trunc);
		m << manifest;
		if (!m) {
			c.state = LsfgState::Error;
			c.error = "cannot write manifest";
			LOG_ERR("LSFG: Cannot write '%s'",
			        manifest_path.string().c_str());
			return;
		}
	}

	c.blob_count  = written;
	c.dxbc_count  = dxbc;
	c.spirv_count = spirv;
	c.state       = LsfgState::ReadyFresh;
	LOG_MSG("LSFG: Extracted %zu shader blobs (%zu DXBC + %zu SPIR-V) "
	        "from '%s' to '%s' (one-time; cached under fingerprint %s)",
	        written, dxbc, spirv, dll_path.c_str(), dir.string().c_str(),
	        fingerprint.c_str());
}

} // namespace

void LSFG_AddKeys(SectionProp& section)
{
	using enum Property::Changeable::Value;

	auto pstring_method = section.AddString("framegen_method", Always, "blend");
	pstring_method->SetHelp(
	        "Frame-generation method for the [framegen] switches ('blend'\n"
	        "by default). 'blend' interpolates the last two emulated frames\n"
	        "on the CPU. 'lsfg' stages the Lossless Scaling shaders from\n"
	        "your own 'framegen_lsfg_dll' copy (extracted once, cached under\n"
	        "the config dir, re-extracted automatically when the DLL\n"
	        "changes) for the upcoming optical-flow dispatch; until that\n"
	        "stage lands, 'lsfg' behaves like 'blend'.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pstring_method->SetValues({"blend", "lsfg"});

	auto ppath_dll = section.AddPath("framegen_lsfg_dll", Always, "");
	ppath_dll->SetHelp(
	        "Full path to your Lossless Scaling 'Lossless.dll' (Steam\n"
	        "default '.../steamapps/common/Lossless Scaling/Lossless.dll').\n"
	        "Set once: the shader blobs are extracted from it a single time\n"
	        "and cached, so the DLL is only re-read when it changes\n"
	        "(e.g. after a Steam update). Only used when 'framegen_method'\n"
	        "is 'lsfg'. The DLL is parsed as data and never executed.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");

	auto pbool_gpu = section.AddBool("framegen_lsfg_gpu", Always, true);
	pbool_gpu->SetHelp(
	        "Run the LSFG optical-flow dispatch on the GPU when\n"
	        "'framegen_method' is 'lsfg' (on by default). Off forces the\n"
	        "CPU blend path even with staged shaders (useful for A/B\n"
	        "comparisons during headset runs).\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");

	auto pbool_fp32 = section.AddBool("framegen_lsfg_fp32", Always, false);
	pbool_fp32->SetHelp(
	        "Use the fp32 LSFG shader variants instead of fp16 (off by\n"
	        "default). Try on if the fp16 chain fails to build on your GPU\n"
	        "(older cards can be slower or stricter with fp16); the chain\n"
	        "falls back to fp32 automatically when fp16 fails.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");

	auto pbool_perf = section.AddBool("framegen_lsfg_perf", Always, false);
	pbool_perf->SetHelp(
	        "Use the lighter performance LSFG model instead of full quality\n"
	        "(off by default). Much faster per frame (aim for this when the\n"
	        "'GPU avg' line exceeds your frame budget) with a slight quality\n"
	        "loss on difficult motion.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");

	auto pint_flow = section.AddInt("framegen_lsfg_flow_scale", Always, 100);
	pint_flow->SetHelp(
	        "Optical-flow working resolution, as a percentage of the frame\n"
	        "(100 by default). The flow is computed at this fraction of the\n"
	        "game resolution, so 25..100 trades speed for how well fast or\n"
	        "fine motion is tracked. Lower this first when the 'GPU avg' line\n"
	        "is too high at your game resolution.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_flow->SetMinMax(25, 100);
}

bool LSFG_EnsureReady()
{
	auto& c      = cache();
	auto section = get_section("framegen");
	const std::string method =
	        section ? section->GetString("framegen_method") : "blend";
	const std::string dll = current_dll_path(section);

	// Re-evaluate at most once per distinct (method, dll) pair, so
	// live config changes re-arm the one-shot while steady state
	// costs two string comparisons per frame.
	if (!c.evaluated || method != c.method_seen || dll != c.dll_seen) {
		c.evaluated   = true;
		c.method_seen = method;
		c.dll_seen    = dll;
		if (method != "lsfg") {
			c.state = LsfgState::Disabled;
			return false;
		}
		if (dll.empty()) {
			c.state = LsfgState::Disabled;
			LOG_MSG("LSFG: 'framegen_method' is 'lsfg' but no DLL is "
			        "set; set [framegen] framegen_lsfg_dll to your "
			        "Lossless Scaling Lossless.dll (one-time setup)");
			return false;
		}
		do_extract(dll);
	}
	return c.state == LsfgState::ReadyCached ||
	       c.state == LsfgState::ReadyFresh;
}

std::string LSFG_StatusText()
{
	const auto& c = cache();
	char buf[256] = {};
	switch (c.state) {
	case LsfgState::Unchecked:
		return "LSFG: not checked yet";
	case LsfgState::Disabled:
		return "LSFG: off (method=blend or no DLL configured)";
	case LsfgState::Error:
		snprintf(buf, sizeof(buf), "LSFG: error (%s)", c.error.c_str());
		return buf;
	case LsfgState::ReadyCached:
		snprintf(buf, sizeof(buf),
		         "LSFG staged: %zu blobs (%zu DXBC + %zu SPIR-V), "
		         "cache hit %s",
		         c.blob_count, c.dxbc_count, c.spirv_count,
		         c.fingerprint.c_str());
		return buf;
	case LsfgState::ReadyFresh:
		snprintf(buf, sizeof(buf),
		         "LSFG staged: %zu blobs (%zu DXBC + %zu SPIR-V), "
		         "extracted once %s",
		         c.blob_count, c.dxbc_count, c.spirv_count,
		         c.fingerprint.c_str());
		return buf;
	}
	return "LSFG: unknown state";
}

std::string LSFG_StagedDir()
{
	const auto& c = cache();
	if (c.state != LsfgState::ReadyCached &&
	    c.state != LsfgState::ReadyFresh) {
		return {};
	}
	return (get_config_dir() / "lsfg" / c.fingerprint).string();
}
