// SPDX-FileCopyrightText:  2020-2025 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "port.h"

#include <cassert>
#include <cstring>
#include <functional>
#include <limits>
#include <unordered_map>

#include "config/setup.h"
#include "misc/support.h"
#include "timer.h"

// Forward declaration of the handler maps defined below; the lazy VIP
// echo installer (also below, but inside the anonymous namespace needs
// it earlier) indexes lane 0, the byte lane.
extern std::unordered_map<io_port_t, io_read_f> io_read_handlers[];

void IO_ReadHandleObject::Uninstall(){
	if(!installed) return;

	IO_FreeReadHandler(m_port, m_width, m_range);
	installed = false;
}

IO_ReadHandleObject::~IO_ReadHandleObject(){
	Uninstall();
}

void IO_WriteHandleObject::Uninstall() {
	if(!installed) return;

	IO_FreeWriteHandler(m_port, m_width, m_range);
	installed = false;
}

IO_WriteHandleObject::~IO_WriteHandleObject(){
	Uninstall();
	// LOG_MSG("IOBUS: FreeWritehandler called with port %04x",
	// static_cast<uint32_t>(m_port));
}

// Stereo/shutter-glasses detection: COM1-4 and LPT1-4 port ranges.
// VFX-1 VIP-card detection: VIP base ports 260h/280h/2A0h/2C0h.
// Accesses are logged when the value changes, so status-polling loops
// stay silent but per-frame sync toggles (e.g. eye-select flips) show
// up with port, direction, register, and value.
namespace {

constexpr io_port_t StereoComBases[] = {0x3f8, 0x2f8, 0x3e8, 0x2e8};
constexpr const char* StereoComNames[] = {"COM1", "COM2", "COM3", "COM4"};
constexpr io_port_t StereoLptBases[] = {0x378, 0x278, 0x3bc, 0x268};
constexpr const char* StereoLptNames[] = {"LPT1", "LPT2", "LPT3", "LPT4"};
constexpr io_port_t Vfx1VipBases[] = {0x260, 0x280, 0x2a0, 0x2c0};

constexpr const char* StereoComRegRead[] =
        {"RHR", "IER", "ISR", "LCR", "MCR", "LSR", "MSR", "SPR"};
constexpr const char* StereoComRegWrite[] =
        {"THR", "IER", "FCR", "LCR", "MCR", "LSR", "MSR", "SPR"};
constexpr const char* StereoLptReg[] = {"data", "status", "control"};

struct StereoPortSlot {
	bool read_valid   = false;
	bool write_valid  = false;
	uint8_t last_read  = 0;
	uint8_t last_write = 0;
};

// 4 COM ports x 8 registers + 4 LPT ports x 3 registers.
constexpr int StereoNumSlots = 4 * 8 + 4 * 3;

static StereoPortSlot stereo_slots[StereoNumSlots] = {};

// 4 VIP bases x 16 registers (offsets +0..+0xF; offsets log raw).
constexpr int Vfx1NumSlots = 4 * 16;

static StereoPortSlot vfx1_slots[Vfx1NumSlots] = {};

// Bit masks (within master/slave IMR byte) of the watched IRQ lines.
constexpr uint8_t Vfx1WatchedIrqMaster = (1u << 5) | (1u << 7);
constexpr uint8_t Vfx1WatchedIrqSlave =
        (1u << (10 - 8)) | (1u << (11 - 8)) | (1u << (12 - 8)) |
        (1u << (15 - 8));

// Classify a port into the tracked ranges. Returns false when the
// port is of no interest (fast path: two comparisons).
static bool stereo_classify_port(const io_port_t port, const bool write,
                                 const char*& dev_name, const char*& reg_name,
                                 int& slot)
{
	if (port < 0x268 || port > 0x3ff) {
		return false;
	}
	for (int i = 0; i < 4; ++i) {
		if (port >= StereoComBases[i] && port < StereoComBases[i] + 8) {
			const auto offset = static_cast<int>(port - StereoComBases[i]);
			dev_name = StereoComNames[i];
			reg_name = write ? StereoComRegWrite[offset]
			                 : StereoComRegRead[offset];
			slot = i * 8 + offset;
			return true;
		}
	}
	for (int i = 0; i < 4; ++i) {
		// Note: 0x3BC-0x3BE overlaps the MDA range; the port address
		// in the message disambiguates.
		if (port >= StereoLptBases[i] && port < StereoLptBases[i] + 3) {
			const auto offset = static_cast<int>(port - StereoLptBases[i]);
			dev_name = StereoLptNames[i];
			reg_name = StereoLptReg[offset];
			slot     = 4 * 8 + i * 3 + offset;
			return true;
		}
	}
	return false;
}

static uint8_t stereo_shutter_value = 0x00;
static bool stereo_shutter_dirty = false;
static bool stereo_port_logging_enabled(const bool is_com)
{
	// Live toggle from the [xr] section (read only for in-range
	// ports, so the hot path is unaffected). Missing section means
	// fail-open: diagnostics default on.
	const auto* section = get_section("xr");
	if (!section) {
		return true;
	}
	return section->GetBool(is_com ? "xr_log_com" : "xr_log_lpt");
}

static void log_stereo_port_access(const io_port_t port, const bool write,
                                   const uint8_t value)
{
	const char* dev_name = nullptr;
	const char* reg_name = nullptr;
	int slot             = -1;
	if (!stereo_classify_port(port, write, dev_name, reg_name, slot)) {
		return;
	}
	if (!stereo_port_logging_enabled(slot < 4 * 8)) {
		return;
	}
	auto& entry = stereo_slots[slot];
	if (write) {
		if (entry.write_valid && entry.last_write == value) {
			return; // steady state stays silent
		}
		entry.write_valid = true;
		entry.last_write  = value;
	} else {
		if (entry.read_valid && entry.last_read == value) {
			return;
		}
		entry.read_valid = true;
		entry.last_read  = value;
	}
	LOG_MSG("STEREO3D: %s %s %s 0x%03x = 0x%02x",
	        dev_name,
	        write ? "write" : "read",
	        reg_name,
	        port,
	        value);
}

// VFX-1 VIP-card detection: base ports 260h/280h/2A0h/2C0h (the four
// VIPPORT jumper options). The VIP register map spans +0..+0xF
// (VFX1.H: AIR +0x0, HWID +0xC, HWVLOW +0xD, HWVHI +0xE, CFGSTATUS
// +0xF), so slots are 16 wide; offsets log raw. Same on-change
// semantics as above.
static bool vfx1_classify_port(const io_port_t port, int& slot)
{
	if (port < 0x260 || port > 0x2cf) {
		return false;
	}
	for (int i = 0; i < 4; ++i) {
		if (port >= Vfx1VipBases[i] && port < Vfx1VipBases[i] + 16) {
			slot = i * 16 + static_cast<int>(port - Vfx1VipBases[i]);
			return true;
		}
	}
	return false;
}

static bool vfx1_logging_enabled()
{
	// Live toggle from the [xr] section. Missing section means
	// fail-open: diagnostics default on (same as the LPT/COM spies).
	const auto* section = get_section("xr");
	if (!section) {
		return true;
	}
	return section->GetBool("xr_log_vip");
}

static void log_vfx1_port_access(const io_port_t port, const bool write,
                                 const uint8_t value)
{
	int slot = -1;
	if (!vfx1_classify_port(port, slot)) {
		return;
	}
	if (!vfx1_logging_enabled()) {
		return;
	}
	auto& entry = vfx1_slots[slot];
	if (write) {
		if (entry.write_valid && entry.last_write == value) {
			return; // steady state stays silent
		}
		entry.write_valid = true;
		entry.last_write  = value;
	} else {
		if (entry.read_valid && entry.last_read == value) {
			return;
		}
		entry.read_valid = true;
		entry.last_read  = value;
	}
	LOG_MSG("VFX1: VIP %s +0x%x (0x%03x) = 0x%02x",
	        write ? "write" : "read",
	        slot % 16,
	        port,
	        value);
}

// Word-wide twin of the line above: fires in addition to the two byte
// lines, so word IO (in ax,dx) is distinguishable from two byte IOs.
// No dedup here; word traffic to these ports is detection-only rare.
static void log_vfx1_port_access_wide(const io_port_t port, const bool write,
                                      const uint16_t value)
{
	int slot = -1;
	if (!vfx1_classify_port(port, slot)) {
		return;
	}
	if (!vfx1_logging_enabled()) {
		return;
	}
	LOG_MSG("VFX1: VIP %s16 +0x%x (0x%03x) = 0x%04x",
	        write ? "write" : "read",
	        slot % 16,
	        port,
	        value);
}

// Register latches for the VIP baseport/init tests, reverse-engineered
// from SETVFX1.EXE/VFX1.COM: +0x0 AIR echoes, +0xC/D/E answer fixed IDs,
// +0x8 is an R/W control register (VFX1.COM does read/OR/AND/write on
// it) whose bit 5 reads BUSY while the DFIN engine has recent traffic
// (the init poll spins on it), +0x9/+0xB latch like AIR. Claimed ports
// keep their real handlers; nothing else is emulated.
static uint8_t vfx1_reg_latch[4][16] = {};
static bool vfx1_reg_valid[4][16]    = {};
// Last DFIN (+0x2) burst time: bit 5 of +0x8 reads busy for a few ms
// after download traffic, then the "device" goes idle.
static int64_t vfx1_dfin_last_us = 0;
static constexpr int64_t Vfx1BusyWindowUs = 5000; // 5 ms

static bool vfx1_respond_enabled()
{
	// Fail-closed: never answer hardware unasked.
	const auto* section = get_section("xr");
	if (!section) {
		return false;
	}
	return section->GetBool("xr_vip_respond");
}

static void vfx1_latch_write(const io_port_t port, const uint8_t value)
{
	int slot = -1;
	if (!vfx1_classify_port(port, slot)) {
		return;
	}
	const int base   = slot / 16;
	const int offset = slot % 16;
	if (offset == 0x2) {
		vfx1_dfin_last_us = GetTicksUs();
		return;
	}
	if (offset != 0x0 && offset != 0x8 && offset != 0x9 &&
	    offset != 0xB) {
		return;
	}
	vfx1_reg_latch[base][offset] = value;
	vfx1_reg_valid[base][offset] = true;
}

// Read handler installed lazily on the first classified touch of an
// unclaimed VIP base: answers echo/ID/latch registers while responding,
// open bus otherwise. Claimed ports never reach here (their handlers
// win), and later claimants overwrite this entry, so real hardware
// always wins.
static io_val_t vfx1_reg_handler(const io_port_t port, const io_width_t)
{
	uint8_t value = 0xff;
	if (vfx1_respond_enabled()) {
		int slot = -1;
		if (vfx1_classify_port(port, slot)) {
			const int base   = slot / 16;
			const int offset = slot % 16;
			switch (offset) {
			case 0x0c: value = 0x0d; break;
			case 0x0d: value = 0x01; break; // any non-FF passes
			case 0x0e: value = 0x01; break; // any non-FF passes
			case 0x0:
			case 0x8:
			case 0x9:
			case 0xB:
				if (vfx1_reg_valid[base][offset]) {
					value = vfx1_reg_latch[base][offset];
					if (offset == 0x8) {
						// Bit 5 is BUSY, not latched.
						value &= ~0x20u;
						if (GetTicksUs() -
						            vfx1_dfin_last_us <
						    Vfx1BusyWindowUs) {
							value |= 0x20u;
						}
					}
				}
				break;
			default: break;
			}
		}
	}
	return value;
}

static void vfx1_maybe_install_base(const io_port_t port)
{
	// Any classified touch installs the base's whole answer set, so
	// probe-first detection (ID reads before any echo write) sees
	// the card on its very first access.
	int slot = -1;
	if (!vfx1_classify_port(port, slot)) {
		return;
	}
	if (!vfx1_respond_enabled()) {
		return;
	}
	// Index 0 is the byte lane (same as io_read_byte_handler below).
	// Echo on base+0, control latch on +0x8/+0x9/+0xB, ID answers on
	// +0xC/+0xD/+0xE; only where nothing is claimed.
	auto& byte_lane = io_read_handlers[0];
	const int base = slot / 16;
	const io_port_t base_port = Vfx1VipBases[base];
	auto install = [&](const int offset) {
		const io_port_t p = base_port + offset;
		if (byte_lane.find(p) == byte_lane.end()) {
			byte_lane[p] = vfx1_reg_handler;
		}
	};
	install(0x0);
	install(0x8);
	install(0x9);
	install(0xB);
	install(0xC);
	install(0xD);
	install(0xE);
}

} // namespace

// External linkage (declared in hardware/port.h): guest PIC mask
// writes call in here with the pre/post mask bytes; transitions on
// the watched IRQ lines (5/7/10/11/12/15) log. Programmatic mask
// changes (emulator init) do not come through here.
void VFX1_NotifyIrqMask(const int pic, const uint8_t old_imr,
                        const uint8_t new_imr)
{
	if (pic < 0 || pic > 1) {
		return;
	}
	if (!vfx1_logging_enabled()) {
		return;
	}
	const uint8_t watched = (pic == 0) ? Vfx1WatchedIrqMaster
	                                   : Vfx1WatchedIrqSlave;
	const uint8_t changed = (old_imr ^ new_imr) & watched;
	for (int bit = 0; bit < 8; ++bit) {
		if (changed & (1u << bit)) {
			const int irq      = (pic == 0) ? bit : bit + 8;
			const bool in_use  = (new_imr & (1u << bit)) == 0;
			LOG_MSG("VFX1: IRQ %d %s",
			        irq,
			        in_use ? "unmasked (in use)" : "masked");
		}
	}
}

// External linkage (declared in hardware/port.h): the shutter state
// above stays file-local; only the consume accessor is shared.
bool STEREO3D_ConsumeLptShutter(uint8_t& value_out)
{
	if (!stereo_shutter_dirty) {
		return false;
	}
	stereo_shutter_dirty = false;
	value_out            = stereo_shutter_value;
	return true;
}

// type-sized IO handlers
std::unordered_map<io_port_t, io_read_f> io_read_handlers[io_widths] = {};
constexpr auto &io_read_byte_handler = io_read_handlers[0];
constexpr auto &io_read_word_handler = io_read_handlers[1];
constexpr auto &io_read_dword_handler = io_read_handlers[2];

std::unordered_map<io_port_t, io_write_f> io_write_handlers[io_widths] = {};
constexpr auto &io_write_byte_handler = io_write_handlers[0];
constexpr auto &io_write_word_handler = io_write_handlers[1];
constexpr auto &io_write_dword_handler = io_write_handlers[2];

constexpr io_val_t blocked_read(const io_port_t, const io_width_t)
{
	return 0xff;
}

// type-sized IO handler API
uint8_t read_byte_from_port(const io_port_t port)
{
	// Answer the first touch of VIP registers: probe-first detection
	// (ID reads before any echo write) must see the card immediately.
	vfx1_maybe_install_base(port);
	const auto [it, was_blocked] = io_read_byte_handler.try_emplace(port, blocked_read);
	if (was_blocked)
		LOG(LOG_IO, LOG_WARN)("Unhandled read from port %04Xh; blocking", port);
	const auto retval = static_cast<uint8_t>(it->second(port, io_width_t::byte) & 0xff);
	log_stereo_port_access(port, false, retval);
	log_vfx1_port_access(port, false, retval);
	return retval;
}

uint16_t read_word_from_port(const io_port_t port)
{
	const auto reader = io_read_word_handler.find(port);
	uint16_t value = 0;
	if (reader != io_read_word_handler.end()) {
		value = check_cast<uint16_t>(reader->second(port, io_width_t::word) &
		                              0xffff);
	} else {
		value = check_cast<uint16_t>(static_cast<io_val_t>(
		        read_byte_from_port(port) |
		        (read_byte_from_port(port + 1) << 8)));
	}
	log_vfx1_port_access_wide(port, false, value);
	return value;
}

uint32_t read_dword_from_port(const io_port_t port)
{
	const auto reader = io_read_dword_handler.find(port);
	const auto value = reader != io_read_dword_handler.end()
	                           ? reader->second(port, io_width_t::dword)
	                           : static_cast<io_val_t>(
	                                     read_word_from_port(port) |
	                                     (read_word_from_port(port + 2) << 16));
	assert(value <= UINT32_MAX);
	return static_cast<uint32_t>(value);
}

constexpr void blocked_write(const io_port_t, const io_val_t, const io_width_t)
{
	// nothing to write to
}

void write_byte_to_port(const io_port_t port, const uint8_t val)
{
	const auto [it, was_blocked] = io_write_byte_handler.try_emplace(port, blocked_write);
	if (was_blocked)
		LOG(LOG_IO, LOG_WARN)("Unhandled write of value 0x%02x"
		                      " (%u) to port %04Xh; blocking",
		                      val, val, port);
	it->second(port, val, io_width_t::byte);
	log_stereo_port_access(port, true, val);
	log_vfx1_port_access(port, true, val);
	vfx1_latch_write(port, val);
	// Answer future reads of unclaimed VIP base ports from the latch
	// (installed once; real handlers always win by registration order).
	vfx1_maybe_install_base(port);
	if (port == 0x378) {
		stereo_shutter_value = val;
		stereo_shutter_dirty = true;
	}
}

void write_word_to_port(const io_port_t port, const uint16_t val)
{
	const auto writer = io_write_word_handler.find(port);
	if (writer != io_write_word_handler.end()) {
		writer->second(port, val, io_width_t::word);
	} else {
		write_byte_to_port(port, static_cast<uint8_t>(val & 0xff));
		write_byte_to_port(port + 1, static_cast<uint8_t>(val >> 8));
	}
	log_vfx1_port_access_wide(port, true, val);
}

void write_dword_to_port(const io_port_t port, const uint32_t val)
{
	const auto writer = io_write_dword_handler.find(port);
	if (writer != io_write_dword_handler.end()) {
		writer->second(port, val, io_width_t::dword);
	} else {
		write_word_to_port(port, static_cast<uint16_t>(val & 0xffff));
		write_word_to_port(port + 2, static_cast<uint16_t>(val >> 16));
	}
}

void IO_RegisterReadHandler(io_port_t port,
                            const io_read_f handler,
                            const io_width_t max_width,
                            io_port_t range)
{
	while (range--) {
		io_read_byte_handler[port] = handler;
		if (max_width == io_width_t::word || max_width == io_width_t::dword)
			io_read_word_handler[port] = handler;
		if (max_width == io_width_t::dword)
			io_read_dword_handler[port] = handler;
		++port;
	}
}

void IO_RegisterWriteHandler(io_port_t port,
                             const io_write_f handler,
                             const io_width_t max_width,
                             io_port_t range)
{
	while (range--) {
		io_write_byte_handler[port] = handler;
		if (max_width == io_width_t::word || max_width == io_width_t::dword)
			io_write_word_handler[port] = handler;
		if (max_width == io_width_t::dword)
			io_write_dword_handler[port] = handler;
		++port;
	}
}

void IO_FreeReadHandler(io_port_t port,
                        const io_width_t max_width,
                        io_port_t range)
{
	while (range--) {
		io_read_byte_handler.erase(port);
		if (max_width == io_width_t::word || max_width == io_width_t::dword)
			io_read_word_handler.erase(port);
		if (max_width == io_width_t::dword)
			io_read_dword_handler.erase(port);
		++port;
	}
}

void IO_FreeWriteHandler(io_port_t port,
                         const io_width_t width,
                         io_port_t range)
{
	while (range--) {
		io_write_byte_handler.erase(port);
		if (width == io_width_t::word || width == io_width_t::dword)
			io_write_word_handler.erase(port);
		if (width == io_width_t::dword)
			io_write_dword_handler.erase(port);
		++port;
	}
}

void IO_ReadHandleObject::Install(const io_port_t port,
                                  const io_read_f handler,
                                  const io_width_t max_width,
                                  const io_port_t range)
{
	if (!installed) {
		installed = true;
		m_port = port;
		m_width = max_width;
		m_range = range;
		IO_RegisterReadHandler(port, handler, max_width, range);
	} else
		E_Exit("io_read_f already installed port %u", port);
}

void IO_WriteHandleObject::Install(const io_port_t port,
                                   const io_write_f handler,
                                   const io_width_t max_width,
                                   const io_port_t range)
{
	if (!installed) {
		installed = true;
		m_port = port;
		m_width = max_width;
		m_range = range;
		IO_RegisterWriteHandler(port, handler, max_width, range);
	} else
		E_Exit("io_write_f already installed port %u", port);
}
