// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// SU-2000 GDT-corruption watch and guard.
//
// The card-image fill (ETS far-memset at file 0x4A14A) sizes itself from an
// uninitialised object field, so it writes ~360 KB of filler from the card
// memory window - a selector whose base lies in the ISA range
// (0x80000-0xFFFFF). On real hardware writes beyond the 16 KB card window
// vanish into the ISA bus; in DOSBox they land in system RAM and smash the
// DOS extender's GDT (observed at 0x110010), aborting ETS at the memset's
// trailing POP ES. The guard serves the extender's extended RAM through a
// pass-through filter that DROPS any write made through an ISA-range segment
// (the card window), while the extender's own flat-selector writes pass
// normally. Installed when the guest loads the GDT via LGDT and torn down on
// uninstall.

#include "hardware/su2000.h"
#include "hardware/su2000_internal.h"

#include <cstdint>
#include <vector>

#include "cpu/cpu.h"
#include "cpu/paging.h"
#include "misc/support.h"

namespace {

// GDT-corruption watch: the crash post-mortem showed GDT[0x2D] (selector
// 0x168) smashed with 0x52 bytes by an earlier wild guest stos, surfacing
// only later as #NP at a far-memset pop-es. Snapshot the descriptor on the
// first emulated access per session and report the first change with the
// guest position, bracketing the smasher between two logged events.
struct Su2000GdtSnap {
	bool valid    = false;
	bool reported = false;
	uint32_t base = 0;
	uint32_t limit = 0;
	uint8_t dtype = 0;
	uint8_t dpl   = 0;
	bool present  = false;
};

Su2000GdtSnap gdt_snap = {};

bool gdt_guard_wanted    = false;
bool gdt_guard_installed = false;
uint32_t gdt_guard_lo    = 0; // first guarded page
uint32_t gdt_guard_hi    = 0; // last guarded page (inclusive)

class Su2000GdtGuardHandler;

Su2000GdtGuardHandler* gdt_guard_handler = nullptr;
std::vector<PageHandler*> gdt_guard_saved = {};

class Su2000GdtGuardHandler final : public PageHandler {
public:
	Su2000GdtGuardHandler(const std::vector<PageHandler*>* saved, const uint32_t lo)
	        : saved_handlers(saved),
	          guard_lo(lo)
	{
		// READABLE lets the guest keep fetching/reading the guarded RAM
		// (some of it is extender code); writes are NOT advertised so every
		// store is routed through writeb/writew/writed below.
		flags = PFLAG_READABLE;
	}
	uint8_t readb(PhysPt addr) override { return phys_readb(addr); }
	uint16_t readw(PhysPt addr) override { return phys_readw(addr); }
	uint32_t readd(PhysPt addr) override { return phys_readd(addr); }
	HostPt GetHostReadPt(Bitu phys_page) override
	{
		const Bitu idx = phys_page - guard_lo;
		if (saved_handlers && idx < saved_handlers->size() &&
		    (*saved_handlers)[idx]) {
			return (*saved_handlers)[idx]->GetHostReadPt(phys_page);
		}
		return PageHandler::GetHostReadPt(phys_page);
	}
	HostPt GetHostWritePt(Bitu phys_page) override
	{
		return GetHostReadPt(phys_page);
	}
	void writeb(PhysPt addr, uint8_t val) override { store(addr, val, 1); }
	void writew(PhysPt addr, uint16_t val) override { store(addr, val, 2); }
	void writed(PhysPt addr, uint32_t val) override { store(addr, val, 4); }

private:
	void store(const PhysPt addr, const uint32_t val, const int width)
	{
		if (card_window_overrun(addr)) {
			++dropped;
			if (su2000_log_enabled() &&
			    (dropped <= 32 || (dropped & 0x3FFF) == 0)) {
				LOG_MSG("SU2000: GDT guard drop #%u addr %08x page %03x "
				        "val %x w%d CS:EIP %04x:%08x es %04x(%08x) "
				        "ds %04x(%08x) edi %08x ecx %08x",
				        static_cast<unsigned>(dropped),
				        static_cast<unsigned>(addr),
				        static_cast<unsigned>(addr >> 12),
				        static_cast<unsigned>(val),
				        width,
				        static_cast<unsigned>(SegValue(cs)),
				        static_cast<unsigned>(reg_eip),
				        static_cast<unsigned>(SegValue(es)),
				        static_cast<unsigned>(SegPhys(es)),
				        static_cast<unsigned>(SegValue(ds)),
				        static_cast<unsigned>(SegPhys(ds)),
				        static_cast<unsigned>(reg_edi),
				        static_cast<unsigned>(reg_ecx));
			}
			return;
		}
		switch (width) {
		case 1: phys_writeb(addr, static_cast<uint8_t>(val)); break;
		case 2: phys_writew(addr, static_cast<uint16_t>(val)); break;
		default: phys_writed(addr, val); break;
		}
	}

	const std::vector<PageHandler*>* saved_handlers = nullptr;
	uint32_t guard_lo = 0;
	uint32_t dropped  = 0;
	// The SU-2000 image buffers are reached through the NET card's 16 KB
	// memory window (J10: C0000-DE000, configured C8000). A store through a
	// selector based inside that window whose linear target lands past the
	// 16 KB the ISA card decodes is a wild overrun: on real hardware it
	// disappears into the bus, in DOSBox it would land in extended RAM. Only
	// ES matters - the offending access is a REP STOS.
	static bool card_window_overrun(const PhysPt addr)
	{
		const PhysPt base = SegPhys(es);
		return base >= 0xC0000u && base < 0xE0000u &&
		       addr >= base + 0x4000u;
	}
};

void su2000_unguard_gdt()
{
	if (!gdt_guard_installed) {
		return;
	}
	uint32_t page = gdt_guard_lo;
	for (auto* saved : gdt_guard_saved) {
		if (saved) {
			MEM_SetPageHandler(page, 1, saved);
		} else {
			MEM_ResetPageHandler(page, 1);
		}
		++page;
	}
	gdt_guard_saved.clear();
	gdt_guard_installed = false;
	gdt_guard_lo = gdt_guard_hi = 0;
	PAGING_ClearTLB();
	delete gdt_guard_handler;
	gdt_guard_handler = nullptr;
}

void su2000_guard_gdt(const uint32_t base, const uint32_t limit)
{
	if (!gdt_guard_wanted || base == 0) {
		return;
	}
	// Only meaningful for an extender GDT parked in extended RAM; a
	// real-mode/IVT-level table has no safe pass-through target here.
	const uint32_t gdt_lo = base >> 12;
	const uint32_t gdt_hi = (base + limit) >> 12;
	if (gdt_lo < 0x100u) {
		return;
	}
	// Cover the GDT plus the extended-RAM window the wild fill can reach
	// (observed end ~0x122200; the GDT limit keeps margin if it moves).
	const uint32_t lo = 0x100u;
	const uint32_t hi = (gdt_hi > 0x140u) ? gdt_hi : 0x140u;
	if (gdt_guard_installed && lo == gdt_guard_lo && hi == gdt_guard_hi) {
		return;
	}
	su2000_unguard_gdt();
	gdt_guard_saved.reserve(hi - lo + 1);
	gdt_guard_handler = new Su2000GdtGuardHandler(&gdt_guard_saved, lo);
	for (uint32_t p = lo; p <= hi; ++p) {
		gdt_guard_saved.push_back(MEM_GetPageHandler(p));
		MEM_SetPageHandler(p, 1, gdt_guard_handler);
	}
	gdt_guard_lo        = lo;
	gdt_guard_hi        = hi;
	gdt_guard_installed = true;
	PAGING_ClearTLB();
	if (su2000_log_enabled()) {
		LOG_MSG("SU2000: GDT guard pages %03x-%03x (GDT 0x%08x+0x%x): "
		        "drops ISA-window writes into extender RAM",
		        lo,
		        hi,
		        base,
		        limit);
	}
}

} // namespace

void SU2000_GDT_SetWanted(const bool wanted)
{
	gdt_guard_wanted = wanted;
}

void SU2000_GDT_Uninstall()
{
	gdt_snap         = Su2000GdtSnap{};
	gdt_guard_wanted = false;
	su2000_unguard_gdt();
}

void su2000_watch_gdt()
{
	Descriptor desc;
	if (!cpu.gdt.GetDescriptor(0x168, desc)) {
		return;
	}
	const uint32_t base = desc.GetBase();
	const uint32_t limit = desc.GetLimit();
	const uint8_t dtype  = desc.Type();
	const uint8_t dpl    = desc.DPL();
	const bool present   = desc.saved.seg.p;
	if (!gdt_snap.valid) {
		gdt_snap.valid   = true;
		gdt_snap.base    = base;
		gdt_snap.limit   = limit;
		gdt_snap.dtype   = dtype;
		gdt_snap.dpl     = dpl;
		gdt_snap.present = present;
		return;
	}
	if (!gdt_snap.reported &&
	    (base != gdt_snap.base || limit != gdt_snap.limit ||
	     dtype != gdt_snap.dtype || dpl != gdt_snap.dpl ||
	     present != gdt_snap.present)) {
		gdt_snap.reported = true;
		LOG_MSG("SU2000: GDT[168] CHANGED was base 0x%08x lim 0x%05x type 0x%x dpl %d p %d now base 0x%08x lim 0x%05x type 0x%x dpl %d p %d [%04x:%04x]",
		        gdt_snap.base,
		        gdt_snap.limit,
		        gdt_snap.dtype,
		        gdt_snap.dpl,
		        gdt_snap.present,
		        base,
		        limit,
		        dtype,
		        dpl,
		        present,
		        SegValue(cs),
		        reg_eip & 0xffffu);
	}
}

void SU2000_OnGdtLoad(const uint32_t base, const uint32_t limit)
{
	su2000_guard_gdt(base, limit);
}
