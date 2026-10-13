// SPDX-FileCopyrightText:  2020-2025 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "port.h"
#include "vfx1.h"

#include <cassert>
#include <cstring>
#include <functional>
#include <limits>
#include <unordered_map>
#include <vector>

#include "config/setup.h"
#include "cpu/cpu.h"
#include "misc/support.h"
#include "pic.h"
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

// Stereo/shutter-glasses detection: COM1-4 and LPT1-3 port ranges
// (no LPT4: 0x268 belongs to the VIP card, never stereo).
// VFX-1 VIP-card detection: VIP base ports 260h/280h/2A0h/2C0h.
// Accesses are logged when the value changes, so status-polling loops
// stay silent but per-frame sync toggles (e.g. eye-select flips) show
// up with port, direction, register, and value.
namespace {

constexpr io_port_t StereoComBases[] = {0x3f8, 0x2f8, 0x3e8, 0x2e8};
constexpr const char* StereoComNames[] = {"COM1", "COM2", "COM3", "COM4"};
constexpr io_port_t StereoLptBases[] = {0x378, 0x278, 0x3bc};
constexpr const char* StereoLptNames[] = {"LPT1", "LPT2", "LPT3"};

constexpr const char* StereoComRegRead[] =
        {"RHR", "IER", "ISR", "LCR", "MCR", "LSR", "MSR", "SPR"};
constexpr const char* StereoComRegWrite[] =
        {"THR", "IER", "FCR", "LCR", "MCR", "LSR", "MSR", "SPR"};
constexpr const char* StereoLptReg[] = {"data", "status", "control"};


// 4 COM ports x 8 registers + 3 LPT ports x 3 registers.
constexpr int StereoNumSlots = 4 * 8 + 3 * 3;

static StereoPortSlot stereo_slots[StereoNumSlots] = {};


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
	for (int i = 0; i < 3; ++i) {
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
// CyberShades shutter state (see STEREO3D_ConsumeCyShutter): latest
// LPT1-3 data or COM1-4 MCR value, with its source (0 = LPT, 1 = COM).
static uint8_t cy_shutter_value = 0x00;
static int cy_shutter_source = 0;
static bool cy_shutter_dirty = false;
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


// ----
// VFX-1 VIP-card emulation (bases, slots, classifier, responder,
// FIFOs, IRQ, install, logging, VFX1_NotifyIrqMask) lives in
// hardware/vfx1.cpp now (moved verbatim); the IO system and stereo
// spies stay here.
// ----


} // namespace


// (VFX1_LogPollCounts lives in hardware/vfx1.cpp now.)


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

// CyberShades consume accessor (declared in hardware/port.h).
bool STEREO3D_ConsumeCyShutter(uint8_t& value_out, int& source_out)
{
	if (!cy_shutter_dirty) {
		return false;
	}
	cy_shutter_dirty = false;
	value_out        = cy_shutter_value;
	source_out       = cy_shutter_source;
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
	// NOTE: no EOI handling here anymore. PIC EOI completion routes
	// through VFX1_NotifyEOI in the PIC layer, which knows which IRQ
	// was actually serviced (a blanket lower on any EOI wiped the
	// pending VIP edge on every timer tick).
	if (port == 0x378) {
		stereo_shutter_value = val;
		stereo_shutter_dirty = true;
	}
	// CyberShades shutter spy: LPT1-3 data ports plus COM1-4 modem
	// control registers (base + 4, where the serial shutter bits
	// live). Independent of the LPT1-only tracker above.
	if (port == 0x378 || port == 0x278 || port == 0x3bc) {
		cy_shutter_value  = val;
		cy_shutter_source = 0;
		cy_shutter_dirty  = true;
	} else if (port == 0x3fc || port == 0x2fc || port == 0x3ec ||
	           port == 0x2ec) {
		cy_shutter_value  = val;
		cy_shutter_source = 1;
		cy_shutter_dirty  = true;
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
