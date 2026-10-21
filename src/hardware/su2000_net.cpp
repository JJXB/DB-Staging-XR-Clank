// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// SU-2000 NET card tracer (temporary bring-up aid).
//
// The SU-2000 NET block (CONFIG [NET] IO 0x280, MEM 0xC8000, IRQ 5) is a
// memory-windowed NE2000/DP8390 clone. Until it is emulated this trap just
// records the access pattern so the register protocol can be reverse
// engineered from ETS's net library.

#include "hardware/su2000_net.h"

#include "hardware/port.h"
#include "hardware/su2000_internal.h"

#include <array>
#include <cstdint>

#include "config/setup.h"
#include "cpu/cpu.h"
#include "cpu/paging.h"
#include "misc/support.h"

// Defined in src/hardware/port.cpp; gates the bring-up low-port I/O watch.
extern uint16_t su2000_net_watch_lo;
extern uint16_t su2000_net_watch_hi;

namespace {

uint16_t net_io_base  = 0;
uint32_t net_mem_base = 0;
IO_ReadHandleObject net_io_read   = {};
IO_WriteHandleObject net_io_write = {};

struct Su2000NetLog {
	uint32_t last   = 0xFFFFFFFFu;
	char dir        = '?';
	unsigned repeat = 0;
	bool valid      = false;
};

Su2000NetLog net_log = {};

void su2000_net_trace(const char dir, const uint32_t addr, const uint32_t value)
{
	if (!su2000_log_enabled()) {
		return;
	}
	if (net_log.valid && value == net_log.last && dir == net_log.dir) {
		++net_log.repeat;
		return;
	}
	if (net_log.valid && net_log.repeat > 0) {
		LOG_MSG("SU2000: NET previous access repeated x%u", net_log.repeat);
	}
	net_log.last   = value;
	net_log.dir    = dir;
	net_log.repeat = 0;
	net_log.valid  = true;
	LOG_MSG("SU2000: NET %c 0x%05x = 0x%02x [%04x:%04x]",
	        dir,
	        addr,
	        value,
	        SegValue(cs),
	        reg_eip & 0xffffu);
}

io_val_t su2000_net_read(io_port_t port, io_width_t)
{
	su2000_watch_gdt();
	su2000_net_trace('R', port, 0xFFu);
	return 0xFF;
}

void su2000_net_write(io_port_t port, io_val_t val, io_width_t)
{
	su2000_watch_gdt();
	su2000_net_trace('W', port, static_cast<uint32_t>(val & 0xFFu));
}

class Su2000NetMemHandler final : public PageHandler {
public:
	explicit Su2000NetMemHandler(const uint32_t base)
	        : mem_base(base)
	{
		ram.fill(0xFF);
		flags = PFLAG_NOCODE; // route every access through readb/writeb
	}
	uint8_t readb(PhysPt addr) override
	{
		const uint8_t v = ram[addr - mem_base];
		su2000_net_trace('R', addr, v);
		return v;
	}
	void writeb(PhysPt addr, uint8_t val) override
	{
		ram[addr - mem_base] = val;
		su2000_net_trace('W', addr, val);
	}

private:
	uint32_t mem_base = 0;
	std::array<uint8_t, 0x4000> ram = {};
};

Su2000NetMemHandler* net_mem_handler = nullptr;

} // namespace

void SU2000_Net_AddConfig(SectionProp& section)
{
	using enum Property::Changeable::Value;

	auto pbool = section.AddBool("net_trap", OnlyAtStart, false);
	pbool->SetHelp(
	        "Log-only trap for the SU-2000 NET block ('off' by default).\n"
	        "Reserves the DP8390 IO block (base 'net_io', 32 ports) and the\n"
	        "16 KB card RAM window (base 'net_mem') and logs every access\n"
	        "with CS:IP so ETS's net-library register protocol is visible.\n"
	        "Reads return 0xFF; the window is RAM-backed from 0xFF.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	auto phex = section.AddHex("net_io", OnlyAtStart, 0x280);
	phex->SetHelp(
	        "SU-2000 NET DP8390 IO base (280 in CONFIG.VPC [NET] IO 280).\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	phex = section.AddHex("net_mem", OnlyAtStart, 0xC8000);
	phex->SetHelp(
	        "SU-2000 NET 16 KB card-RAM window base (C8000 in CONFIG.VPC\n"
	        "[NET] MEM C8000). Must be 16 KB aligned inside C0000-EFFFF.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");
}

bool SU2000_Net_Install(const SectionProp& section)
{
	if (!section.GetBool("net_trap")) {
		return false;
	}
	const auto nio  = static_cast<uint16_t>(section.GetHex("net_io"));
	const auto nmem = static_cast<uint32_t>(section.GetHex("net_mem"));
	const bool io_ok  = nio >= 0x100 && nio <= 0x3E0;
	const bool mem_ok = nmem >= 0xC0000 && nmem <= 0xEFFFF &&
	                    (nmem & 0x3FFFu) == 0;
	if (!io_ok || !mem_ok) {
		LOG_WARNING("SU2000: net trap needs IO 100-3F0 and a 16 KB "
		            "aligned MEM in C0000-EFFFF (got IO 0x%x MEM "
		            "0x%05x); NET trap not installed",
		            nio,
		            nmem);
		return false;
	}
	net_io_base  = nio;
	net_mem_base = nmem;
	net_io_read.Install(nio, su2000_net_read, io_width_t::dword, 32);
	net_io_write.Install(nio, su2000_net_write, io_width_t::dword, 32);
	net_mem_handler = new Su2000NetMemHandler(nmem);
	MEM_SetPageHandler(nmem >> 12, 0x04, net_mem_handler);
	PAGING_ClearTLB();
	su2000_net_watch_lo = 0x0000u;
	su2000_net_watch_hi = 0x03FFu;
	LOG_MSG("SU2000: NET trap build %s (IO 0x%03x MEM 0x%05x "
	        "watch 0x%03x-0x%03x)",
	        SU2000_BUILD_ID,
	        net_io_base,
	        net_mem_base,
	        su2000_net_watch_lo,
	        su2000_net_watch_hi);
	return true;
}

void SU2000_Net_Uninstall()
{
	net_io_read.Uninstall();
	net_io_write.Uninstall();
	su2000_net_watch_lo = 0xFFFFu;
	su2000_net_watch_hi = 0x0000u;
	if (net_mem_handler) {
		MEM_ResetPageHandler(net_mem_base >> 12, 0x04);
		delete net_mem_handler;
		net_mem_handler = nullptr;
	}
	net_mem_base = 0;
	net_io_base  = 0;
	net_log      = Su2000NetLog{};
}

bool SU2000_Net_Installed()
{
	return net_io_base != 0;
}

uint16_t SU2000_Net_IoBase()
{
	return net_io_base;
}
