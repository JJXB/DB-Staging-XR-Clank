// vpc_trace.cpp
// Port-I/O and upper-memory access tracer for Virtuality SU2000 bring-up.
// Drop into src/hardware/ and add to that directory's meson.build.
//
// NOTE: API names follow recent DOSBox Staging trees (io_width_t style handlers).
// Older trees use `Bitu handler(Bitu port, Bitu iolen)`. Check include/inout.h
// and include/paging.h in your checkout and adjust signatures to match.
//
// Enable at runtime with:  VPC_TRACE=vpc.log
//
// Required dosbox config while tracing (to avoid handler/address clashes):
//   cpu: cputype = 486, core = normal (reg_eip is most reliable there)
//   dos: ems = false            (EMS page frame sits at 0xE0000 by default)
//   mpu401 = none               (default base 0x330 clashes with the sound config)
//   ne2000 = off, gus = false   (0x300 / 0x240 ranges)

#include "dosbox.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "inout.h"
#include "memory.h"
#include "paging.h"
#include "pic.h"
#include "regs.h"
#include "setup.h"

namespace {

// ---------------------------------------------------------------------------
// Ranges taken from the sample CONFIG.VPC. Lengths are GUESSES from address
// spacing (player 2 is typically +8 or +0x20); refine as the traces show.
// ---------------------------------------------------------------------------
struct PortRange {
	const char* name;
	io_port_t base;
	uint16_t len;
};

constexpr PortRange port_ranges[] = {
        {"FMTC1", 0x210, 8},    {"FMTC2", 0x218, 8},   {"TRK1", 0x270, 8},
        {"TRK2", 0x278, 8},     {"PROC1", 0x300, 0x10}, {"BFIFO", 0x320, 0x10},
        {"SND1", 0x330, 8},     {"VID1", 0x340, 0x10},  {"SND2", 0x350, 8},
        {"PROC2", 0x360, 0x10},
};

// Upper-memory windows: processor card dual-port RAM (0xD0000..) and
// Format C (0xE0000, 0xE0800 -> both live in one 4K page).
constexpr uint32_t mem_base  = 0xD0000;
constexpr uint32_t mem_size  = 0x20000; // D0000-EFFFF, indexed below
constexpr Bitu first_page_d  = 0xD0;    // 16 pages = D0000-DFFFF
constexpr Bitu pages_d       = 0x10;
constexpr Bitu first_page_e  = 0xE0;    // 1 page   = E0000-E0FFF
constexpr Bitu pages_e       = 1;

const char* range_name(io_port_t port)
{
	for (const auto& r : port_ranges) {
		if (port >= r.base && port < r.base + r.len) {
			return r.name;
		}
	}
	return "?";
}

// ---------------------------------------------------------------------------
// Log writer. Collapses runs of identical accesses (status-register polling
// would otherwise flood the file).
// ---------------------------------------------------------------------------
class Tracer {
public:
	void open(const char* path) { file = std::fopen(path, "w"); }
	bool enabled() const { return file != nullptr; }

	void log(char dir, const char* name, uint32_t addr, uint32_t val, int bytes)
	{
		if (!file) {
			return;
		}
		const uint32_t eip = static_cast<uint32_t>(reg_eip);
		const uint16_t csv = static_cast<uint16_t>(SegValue(cs));

		if (have_last && dir == last.dir && addr == last.addr &&
		    val == last.val && bytes == last.bytes && eip == last.eip &&
		    csv == last.cs) {
			++repeat;
			return;
		}
		flush_repeat();
		std::fprintf(file, "t=%.3f %04x:%08x %c %-5s %06x w%d = %0*x\n",
		             PIC_FullIndex(), csv, eip, dir, name, addr, bytes,
		             bytes * 2, val);
		last = {dir, addr, val, bytes, eip, csv};
		have_last = true;
	}

	void close()
	{
		flush_repeat();
		if (file) {
			std::fclose(file);
			file = nullptr;
		}
	}

private:
	struct Entry {
		char dir;
		uint32_t addr, val;
		int bytes;
		uint32_t eip;
		uint16_t cs;
	};

	void flush_repeat()
	{
		if (repeat && file) {
			std::fprintf(file, "   ... previous access repeated x%u\n", repeat);
		}
		repeat = 0;
	}

	FILE* file = nullptr;
	Entry last = {};
	bool have_last = false;
	unsigned repeat = 0;
};

Tracer tracer;

// ---------------------------------------------------------------------------
// I/O handlers. The port number is passed in, so one pair of functions serves
// every range. Reads return a floating-bus value until you script real answers.
// ---------------------------------------------------------------------------
int width_bytes(io_width_t w)
{
	return static_cast<int>(w); // byte=1, word=2, dword=4 in current trees
}

uint32_t floating_bus(io_width_t w)
{
	switch (w) {
	case io_width_t::byte: return 0xff;
	case io_width_t::word: return 0xffff;
	default: return 0xffffffff;
	}
}

io_val_t trace_io_read(io_port_t port, io_width_t width)
{
	// TODO: look up a scripted response here once you learn a protocol.
	const uint32_t v = floating_bus(width);
	tracer.log('R', range_name(port), port, v, width_bytes(width));
	return v;
}

void trace_io_write(io_port_t port, io_val_t value, io_width_t width)
{
	tracer.log('W', range_name(port), port, value, width_bytes(width));
}

constexpr size_t num_ranges = sizeof(port_ranges) / sizeof(port_ranges[0]);
IO_ReadHandleObject read_handlers[num_ranges];
IO_WriteHandleObject write_handlers[num_ranges];

// ---------------------------------------------------------------------------
// Upper-memory handler. RAM-backed so probe-then-readback tests behave.
// Initial contents are 0xFF (what an empty ISA window reads as).
// ---------------------------------------------------------------------------
class TraceMemHandler final : public PageHandler {
public:
	TraceMemHandler()
	{
		ram.fill(0xff);
		flags = PFLAG_NOCODE; // force every access through readb/writeb
	}

	uint8_t readb(PhysPt addr) override
	{
		const uint8_t v = ram[addr - mem_base];
		tracer.log('R', "MEM", addr, v, 1);
		return v;
	}

	void writeb(PhysPt addr, uint8_t val) override
	{
		ram[addr - mem_base] = val;
		tracer.log('W', "MEM", addr, val, 1);
	}

	// readw/writew/readd/writed fall back to byte accesses in the base class,
	// which keeps the log simple. Override them later if you need wide logging.

private:
	std::array<uint8_t, mem_size> ram;
};

TraceMemHandler* mem_handler = nullptr;

} // namespace

// ---------------------------------------------------------------------------
// Entry points. Register VPCTRACE_Init as an init function on a section that
// initialises AFTER DOS/memory setup (otherwise your page handlers get
// overwritten), then confirm in the debugger that accesses reach the log.
// ---------------------------------------------------------------------------
void VPCTRACE_Destroy(Section*)
{
	tracer.close();
	for (size_t i = 0; i < num_ranges; ++i) {
		read_handlers[i].Uninstall();
		write_handlers[i].Uninstall();
	}
	delete mem_handler;
	mem_handler = nullptr;
}

void VPCTRACE_Init(Section* sec)
{
	const char* path = std::getenv("VPC_TRACE");
	if (!path) {
		return;
	}
	tracer.open(path);
	if (!tracer.enabled()) {
		return;
	}

	for (size_t i = 0; i < num_ranges; ++i) {
		read_handlers[i].Install(port_ranges[i].base, trace_io_read,
		                         io_width_t::dword, port_ranges[i].len);
		write_handlers[i].Install(port_ranges[i].base, trace_io_write,
		                          io_width_t::dword, port_ranges[i].len);
	}

	mem_handler = new TraceMemHandler();
	MEM_SetPageHandler(first_page_d, pages_d, mem_handler);
	MEM_SetPageHandler(first_page_e, pages_e, mem_handler);
	PAGING_ClearTLB();

	sec->AddDestroyFunction(&VPCTRACE_Destroy);
}
