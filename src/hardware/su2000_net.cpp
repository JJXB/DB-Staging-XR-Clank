// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// SU-2000 NET card tracer (temporary bring-up aid).
//
// The SU-2000 NET block (CONFIG [NET] IO 0x280, MEM 0xC8000, IRQ 5) is a
// memory-windowed NE2000/DP8390 clone. Until it is emulated this trap just
// records the access pattern so the register protocol can be reverse
// engineered from ETS's net library.

#include "hardware/su2000_net.h"

#include "hardware/pic.h"
#include "hardware/port.h"
#include "hardware/su2000_internal.h"

#include <array>
#include <cstdint>
#include <cstring>

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
	uint32_t first  = 0;
	uint32_t last   = 0;
	uint8_t value   = 0;
	char dir        = '?';
	unsigned repeat = 0;
	bool valid      = false;
};

Su2000NetLog net_log = {};

// Minimal DP8390 register file so the net library's read-back checks see a
// sane card instead of 0xFF on every port. The RX path is not emulated yet:
// ISR stays 0 and CUR follows BNRY+1, so the driver sees an empty ring.
//
// Station address PROM: 6 MAC bytes + 2 checksum bytes (8-bit sum == 0).
constexpr std::array<uint8_t, 8> net_prom = {
        0x00, 0x80, 0x48, 0x86, 0xF6, 0x21, 0x9B, 0x00};

struct Su2000NetRegs {
	uint8_t cr = 0x21; // stop, remote-DMA complete
	uint8_t pstart = 0;
	uint8_t pstop = 0;
	uint8_t bnry = 0;
	uint8_t tsr = 0;
	uint8_t tpsr = 0;
	uint8_t tbcr0 = 0;
	uint8_t tbcr1 = 0;
	uint8_t rsar0 = 0;
	uint8_t rsar1 = 0;
	uint8_t rbcr0 = 0;
	uint8_t rbcr1 = 0;
	uint8_t rcr = 0;
	uint8_t tcr = 0;
	uint8_t dcr = 0;
	uint8_t imr = 0;
	uint8_t isr = 0;
	std::array<uint8_t, 6> par = {};
	std::array<uint8_t, 8> mar = {};
	uint8_t cur = 0;
	std::array<uint8_t, 8> asic = {0xFF, 0xFF, 0xFF, 0xFF,
	                               0xFF, 0xFF, 0xFF, 0xFF};
};

Su2000NetRegs net_regs = {};

// Backing store shared by the memory window and the NIC's TX/RX logic.
std::array<uint8_t, 0x4000> net_ram = {};

// Virtual peer used when the guest sends a WHO/ECHO request. It answers with
// the same frame layout the real ETS netlib uses (see dumps_network.html).
constexpr std::array<uint8_t, 6> net_peer_mac = {
        0x00, 0x80, 0x48, 0x86, 0xF6, 0x22};

constexpr uint8_t net_fill_byte = 0x54; // 'T' padding used by ETS

void su2000_net_inject(const uint8_t* frame, const uint16_t len)
{
	if (net_regs.pstart >= net_regs.pstop) {
		return;
	}
	const uint16_t count = static_cast<uint16_t>(len + 4);
	const uint8_t pages  = static_cast<uint8_t>((count + 255) / 256);
	// The DP8390 writes at CUR; the netlib reads at BNRY+1 and treats
	// BNRY+1 == CUR as an empty ring. BNRY is PSTART (6) and CUR PSTART+1
	// (7) right after init, so the first packet lands on page 7.
	const uint8_t page = net_regs.cur;
	if (page < net_regs.pstart || page >= net_regs.pstop) {
		return;
	}
	uint8_t next = static_cast<uint8_t>(page + pages);
	if (next >= net_regs.pstop) {
		next = static_cast<uint8_t>(net_regs.pstart +
		                            (next - net_regs.pstop));
	}
	const uint32_t off = static_cast<uint32_t>(page) * 256u;
	const uint32_t end = off + static_cast<uint32_t>(pages) * 256u;
	if (end > net_ram.size()) {
		return;
	}
	net_ram[off + 0] = next;
	net_ram[off + 1] = (frame[0] & 0x01) ? 0x21 : 0x01;
	net_ram[off + 2] = static_cast<uint8_t>(count & 0xFF);
	net_ram[off + 3] = static_cast<uint8_t>(count >> 8);
	std::memcpy(&net_ram[off + 4], frame, len);
	for (uint32_t i = off + 4u + len; i < end; ++i) {
		net_ram[i] = net_fill_byte;
	}
	net_regs.cur = next;
	net_regs.isr |= 0x01;
	if (net_regs.imr & 0x01) {
		PIC_ActivateIRQ(5);
	}
}

// Build a WHO (01 -> 02) or ECHO (03 -> 04) reply for a guest frame. The
// guest's request layout: dest(6) src(6) 00 00 type(2) cmd(1) plen(1) 00(1)
// node-id(6) text(plen-6) ...
bool su2000_net_build_reply(const uint8_t* req, const uint16_t len,
                            std::array<uint8_t, 60>& reply)
{
	if (len < 26 || req[12] != 0 || req[13] != 0) {
		return false;
	}
	const uint8_t cmd = req[16];
	if (cmd != 0x01 && cmd != 0x03) {
		return false;
	}
	const uint8_t plen = req[17];
	if (plen < 6) {
		return false;
	}
	reply.fill(net_fill_byte);
	std::memcpy(&reply[0], &req[6], 6); // dest = guest source
	std::memcpy(&reply[6], net_peer_mac.data(), 6);
	reply[12] = 0;
	reply[13] = 0;
	reply[14] = req[14];
	reply[15] = req[15];
	reply[16] = static_cast<uint8_t>(cmd + 1);
	reply[17] = plen;
	reply[18] = 0;
	std::memcpy(&reply[19], net_peer_mac.data(), 6);
	uint8_t text = static_cast<uint8_t>(plen - 6);
	const uint8_t room = static_cast<uint8_t>(60 - 25);
	if (text > room) {
		text = room;
	}
	std::memcpy(&reply[25], &req[25], text);
	return true;
}

void su2000_net_transmit()
{
	const uint16_t len = static_cast<uint16_t>(net_regs.tbcr0 |
	                                          (net_regs.tbcr1 << 8));
	// The netlib programs TPSR as 0x80 with the frame at offset 0, so the
	// page is masked to the 16 KB window.
	const uint32_t off = static_cast<uint32_t>(net_regs.tpsr & 0x3Fu) * 256u;
	if (len == 0 || off + len > net_ram.size()) {
		return;
	}
	std::array<uint8_t, 60> reply = {};
	if (su2000_net_build_reply(&net_ram[off], len, reply)) {
		su2000_net_inject(reply.data(), static_cast<uint16_t>(reply.size()));
	}
}

uint8_t su2000_net_read_reg(const uint16_t off)
{
	if (off >= 0x08 && off <= 0x0F) {
		return net_prom[off - 0x08];
	}
	if (off < 0x08) {
		return net_regs.asic[off];
	}
	const uint8_t r    = static_cast<uint8_t>(off - 0x10);
	const uint8_t page = static_cast<uint8_t>((net_regs.cr >> 6) & 0x03);
	if (r == 0) {
		return net_regs.cr;
	}
	if (page == 0) {
		switch (r) {
		case 0x01: return 0; // CLDA0
		case 0x02: return 0; // CLDA1
		case 0x03: return net_regs.bnry;
		case 0x04: return net_regs.tsr;
		case 0x05: return 0; // NCR
		case 0x06: return 0; // FIFO
		case 0x07: return net_regs.isr;
		case 0x08: return net_regs.rsar0; // CRDA0
		case 0x09: return net_regs.rsar1; // CRDA1
		case 0x0A: return 0xFF; // reserved
		case 0x0B: return 0xFF; // reserved
		case 0x0C: return 0; // RSR
		case 0x0D: return 0; // CNTR0
		case 0x0E: return 0; // CNTR1
		case 0x0F: return 0; // CNTR2
		default: break;
		}
	} else if (page == 1) {
		if (r >= 0x01 && r <= 0x06) {
			return net_regs.par[r - 0x01];
		}
		if (r == 0x07) {
			return net_regs.cur;
		}
		if (r >= 0x08 && r <= 0x0F) {
			return net_regs.mar[r - 0x08];
		}
	}
	return 0;
}

void su2000_net_write_reg(const uint16_t off, const uint8_t val)
{
	if (off < 0x08) {
		net_regs.asic[off] = val;
		return;
	}
	if (off >= 0x08 && off <= 0x0F) {
		return; // PROM is read-only
	}
	const uint8_t r = static_cast<uint8_t>(off - 0x10);
	if (r == 0) {
		net_regs.cr = val;
		if (((val >> 6) & 0x03) == 0) {
			if (val & 0x04) { // TXP
				net_regs.tsr = 0x01;
				net_regs.isr |= 0x02;
				su2000_net_transmit();
				net_regs.cr &= static_cast<uint8_t>(~0x04);
			}
			const uint8_t rdma = static_cast<uint8_t>((val >> 3) & 0x07);
			if (rdma == 1 || rdma == 2) { // remote DMA completes at once
				net_regs.isr |= 0x40;
				net_regs.cr = static_cast<uint8_t>(
				        (net_regs.cr & ~0x38) | 0x20);
			}
		}
		return;
	}
	const uint8_t page = static_cast<uint8_t>((net_regs.cr >> 6) & 0x03);
	if (page == 0) {
		switch (r) {
		case 0x01: net_regs.pstart = val; break;
		case 0x02: net_regs.pstop = val; break;
		case 0x03: net_regs.bnry = val; break;
		case 0x04: net_regs.tpsr = val; break;
		case 0x05: net_regs.tbcr0 = val; break;
		case 0x06: net_regs.tbcr1 = val; break;
		case 0x07: net_regs.isr &= static_cast<uint8_t>(~val); break;
		case 0x08: net_regs.rsar0 = val; break;
		case 0x09: net_regs.rsar1 = val; break;
		case 0x0A: net_regs.rbcr0 = val; break;
		case 0x0B: net_regs.rbcr1 = val; break;
		case 0x0C: net_regs.rcr = val; break;
		case 0x0D: net_regs.tcr = val; break;
		case 0x0E: net_regs.dcr = val; break;
		case 0x0F: net_regs.imr = val; break;
		default: break;
		}
	} else if (page == 1) {
		if (r >= 0x01 && r <= 0x06) {
			net_regs.par[r - 0x01] = val;
		} else if (r == 0x07) {
			net_regs.cur = val;
		} else if (r >= 0x08 && r <= 0x0F) {
			net_regs.mar[r - 0x08] = val;
		}
	}
}

// Collapse sequential (or same-address) runs of identical byte accesses so a
// card-RAM memset or a status-register poll stays readable in the log. The
// address range is kept, so the run's footprint is still visible.
void su2000_net_flush()
{
	if (!net_log.valid || net_log.repeat == 0) {
		return;
	}
	LOG_MSG("SU2000: NET previous %s repeated x%u (0x%05x-0x%05x = 0x%02x)",
	        net_log.dir == 'R' ? "read" : "write",
	        net_log.repeat,
	        net_log.first,
	        net_log.last,
	        net_log.value);
	net_log.repeat = 0;
}

void su2000_net_trace(const char dir,
                      const uint32_t addr,
                      const uint32_t value,
                      const bool is_code = false)
{
	if (!su2000_log_enabled()) {
		return;
	}
	const auto byte = static_cast<uint8_t>(value & 0xFFu);
	const bool follows = net_log.valid && dir == net_log.dir &&
	                     byte == net_log.value &&
	                     (addr == net_log.last + 1 || addr == net_log.last);
	if (follows) {
		++net_log.repeat;
		net_log.last = addr;
		return;
	}
	su2000_net_flush();
	net_log.first  = addr;
	net_log.last   = addr;
	net_log.value  = byte;
	net_log.dir    = dir;
	net_log.repeat = 0;
	net_log.valid  = true;
	LOG_MSG("SU2000: NET %c 0x%05x = 0x%02x [%04x:%04x] pm%d cs=0x%05x%s",
	        dir,
	        addr,
	        static_cast<unsigned>(byte),
	        SegValue(cs),
	        reg_eip & 0xffffu,
	        cpu.pmode ? 1 : 0,
	        static_cast<unsigned>(SegPhys(cs)),
	        is_code ? " CODE" : "");
}

io_val_t su2000_net_read(io_port_t port, io_width_t)
{
	su2000_watch_gdt();
	const auto off = static_cast<uint16_t>(port - net_io_base);
	const uint8_t value = off < 0x20 ? su2000_net_read_reg(off) : 0xFF;
	su2000_net_trace('R', port, value);
	return value;
}

void su2000_net_write(io_port_t port, io_val_t val, io_width_t)
{
	su2000_watch_gdt();
	const auto off = static_cast<uint16_t>(port - net_io_base);
	const uint8_t value = static_cast<uint8_t>(val & 0xFFu);
	if (off < 0x20) {
		su2000_net_write_reg(off, value);
	}
	su2000_net_trace('W', port, value);
}

class Su2000NetMemHandler final : public PageHandler {
public:
	explicit Su2000NetMemHandler(const uint32_t base)
	        : mem_base(base)
	{
		flags = PFLAG_NOCODE; // route every access through readb/writeb
	}
	uint8_t readb(PhysPt addr) override
	{
		const uint8_t v = net_ram[addr - mem_base];
		// Flag reads of the byte about to be executed as opcode fetch, so a
		// jump into (uninitialised) card RAM is visible in the trace.
		const uint32_t ip = cpu.pmode ? reg_eip : (reg_eip & 0xffffu);
		const bool is_code = addr == static_cast<PhysPt>(SegPhys(cs) + ip);
		su2000_net_trace('R', addr, v, is_code);
		return v;
	}
	void writeb(PhysPt addr, uint8_t val) override
	{
		net_ram[addr - mem_base] = val;
		su2000_net_trace('W', addr, val);
	}

private:
	uint32_t mem_base = 0;
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
	        "The window is RAM-backed and starts filled with 'net_fill'.\n"
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

	phex = section.AddHex("net_fill", OnlyAtStart, 0xFF);
	phex->SetHelp(
	        "Byte that pre-fills the NET card-RAM window (FF by default).\n"
	        "Set to 0 to simulate a cleared SRAM window.\n"
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
	const auto nfill = static_cast<uint8_t>(section.GetHex("net_fill"));
	net_regs = Su2000NetRegs{};
	net_ram.fill(nfill);
	net_io_read.Install(nio, su2000_net_read, io_width_t::dword, 32);
	net_io_write.Install(nio, su2000_net_write, io_width_t::dword, 32);
	net_mem_handler = new Su2000NetMemHandler(nmem);
	MEM_SetPageHandler(nmem >> 12, 0x04, net_mem_handler);
	PAGING_ClearTLB();
	su2000_net_watch_lo = 0x0000u;
	su2000_net_watch_hi = 0x03FFu;
	LOG_MSG("SU2000: NET trap build %s (IO 0x%03x MEM 0x%05x fill 0x%02x "
	        "watch 0x%03x-0x%03x)",
	        SU2000_BUILD_ID,
	        net_io_base,
	        net_mem_base,
	        static_cast<unsigned>(nfill),
	        su2000_net_watch_lo,
	        su2000_net_watch_hi);
	return true;
}

void SU2000_Net_Uninstall()
{
	su2000_net_flush();
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
	net_regs     = Su2000NetRegs{};
	net_ram.fill(0);
}

bool SU2000_Net_Installed()
{
	return net_io_base != 0;
}

uint16_t SU2000_Net_IoBase()
{
	return net_io_base;
}
