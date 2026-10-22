// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Virtuality SU-2000 hardware emulation: core orchestration.
//
// The individual boards live in their own translation units:
//   su2000_formatc.cpp  FORMAT C window + volume registers
//   su2000_tracker.cpp  Polhemus InsideTrak responder
//   su2000_snd.cpp      Ensoniq Soundscape S-2000 host-IO tracer
//   su2000_net.cpp      NET (DP8390) access tracer
//   su2000_traps.cpp    BCST/PIX/VID missing-hardware log-only traps
//   su2000_gdt.cpp      extender-GDT corruption watch/guard
//
// This file owns the shared config section, the ISA ownership map (ports the
// active hardware answers), and install/uninstall orchestration.
//
// Live contract (SU2000.md sections 10-11): CONFIG1.VPC SU solo expects
// FORMAT1 at MEM 0xCC000 IO 0x210; ETS TEST REGISTERS shows the same map;
// MUTE prints "Write 0 to address %lx register %lx" per volume register.
// Success = MUTE runs instead of "Could not open config file" and ETS no
// longer reports "FORMAT-C NOT FOUND" (status reads 0x82).

#include "hardware/su2000.h"

#include "hardware/su2000_formatc.h"
#include "hardware/su2000_internal.h"
#include "hardware/su2000_net.h"
#include "hardware/su2000_snd.h"
#include "hardware/su2000_tracker.h"
#include "hardware/su2000_traps.h"

#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

#include "config/setup.h"
#include "dos/dos.h"
#include "misc/support.h"

namespace {

// ISA ownership map for the VFX-1 snooper guard (see su2000.h): ranges are
// (base, len) pairs recorded at install, cleared at uninstall.
std::vector<std::pair<uint16_t, uint16_t>> claimed_ranges = {};
bool su2000_active = false;

void su2000_clear_claims()
{
	claimed_ranges.clear();
	su2000_active = false;
}

void uninstall()
{
	SU2000_Snd_Uninstall();
	SU2000_Tracker_Uninstall();
	SU2000_Net_Uninstall();
	SU2000_Traps_Uninstall();
	SU2000_FormatC_Uninstall();
	SU2000_GDT_Uninstall();
}

void notify_su2000_setting_updated(SectionProp& section,
                                   const std::string& prop_name)
{
	if (prop_name != "su2000_enable") {
		return;
	}
	const bool enabled = section.GetBool("su2000_enable");

	// The DOS private-table segment is chosen from this key when the
	// tables are first allocated, so a live enable cannot relocate
	// them. The NET card window lives at C8000 when su2000_enable was
	// on at startup; enabling it later leaves the tables where they
	// were, which is fine as long as the NET trap is not enabled.
	if (enabled && DOS_GetPrivateSegment() != DOS_PRIVATE_SEGMENT_SU2000) {
		LOG_WARNING("SU2000: enabled live after boot; DOS tables stay "
		            "at 0x%04x (restart with su2000_enable=on for the "
		            "SU-2000 NET window layout)",
		            DOS_GetPrivateSegment());
	}
	SU2000_SetEnabled(enabled);
	LOG_MSG("SU2000: master switch %s (live)", enabled ? "ON" : "OFF");
}

} // namespace

bool su2000_log_enabled()
{
	const auto* section = get_section("su2000");
	if (!section) {
		return true;
	}
	return section->GetBool("su2000_log");
}

void su2000_claim_range(const uint16_t base, const uint16_t len)
{
	if (base) {
		claimed_ranges.emplace_back(base, len);
	}
}

bool SU2000_ClaimsPort(const uint16_t port)
{
	if (!su2000_active) {
		return false;
	}
	for (const auto& [base, len] : claimed_ranges) {
		if (port >= base && port < base + len) {
			return true;
		}
	}
	return false;
}

void SU2000_AddConfigSection(const ConfigPtr& conf)
{
	assert(conf);

	using enum Property::Changeable::Value;

	auto section = conf->AddSection("su2000");
	section->AddUpdateHandler(notify_su2000_setting_updated);

	auto pbool = section->AddBool("su2000_enable", Always, false);
	pbool->SetHelp(
	        "Master switch for all Virtuality SU-2000 emulation ('off' by\n"
	        "default). When off, nothing is installed regardless of the\n"
	        "other keys below (safe for non-SU-2000 sessions sharing this\n"
	        "config, e.g. VFX-1). When on, each of 'formatc', 'tracker',\n"
	        "'format2', 'bcst_trap', 'pix_trap', 'vid_trap' and 'net_trap'\n"
	        "enables its own piece.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.\n"
	        "The 'SU-2000 on/off' mapper hotkey (Ctrl+Alt+U) toggles it live.\n"
	        "The DOS private-table layout is still selected at startup, so\n"
	        "restart with the key on if you need the NET window at C8000.");

	SU2000_FormatC_AddConfig(*section);

	pbool = section->AddBool("gdt_guard", OnlyAtStart, true);
	pbool->SetHelp(
	        "Guard the DOS extender's GDT from the SU-2000 card-image fill\n"
	        "('on' by default). ETS's card init sizes a far-memset from an\n"
	        "uninitialised field, so it writes ~360 KB of filler through the\n"
	        "card-memory window; on real hardware the excess vanishes into the\n"
	        "ISA bus, but in DOSBox it would land in system RAM and smash the\n"
	        "extender's GDT, aborting ETS. When on, writes reaching extender\n"
	        "RAM through an ISA-window segment are dropped, letting the fill\n"
	        "complete harmlessly.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	SU2000_Traps_AddConfig(*section);
	SU2000_Net_AddConfig(*section);

	pbool = section->AddBool("su2000_log", Always, true);
	pbool->SetHelp(
	        "Log SU-2000 FORMAT C detection and volume traffic ('on' by\n"
	        "default). Healthy polling stays silent; detection-OK, command\n"
	        "writes and volume writes log once each.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");

	SU2000_Tracker_AddConfig(*section);
	SU2000_Snd_AddConfig(*section);
}

void SU2000_Init()
{
	const auto* section = get_section("su2000");
	if (!section) {
		return;
	}
	if (!section->GetBool("su2000_enable")) {
		return; // master switch off: never answer hardware unasked
	}

	// Fail-closed: never answer hardware unasked. At least one board must
	// be explicitly enabled.
	const bool any_board = section->GetBool("formatc") ||
	                       section->GetBool("format2") ||
	                       section->GetBool("tracker") ||
	                       section->GetBool("bcst_trap") ||
	                       section->GetBool("pix_trap") ||
	                       section->GetBool("vid_trap") ||
	                       section->GetBool("net_trap") ||
	                       section->GetBool("snd_trap");
	if (!any_board) {
		return;
	}

	uninstall(); // in case a previous init left state behind
	SU2000_GDT_SetWanted(section->GetBool("gdt_guard"));
	su2000_clear_claims();

	SU2000_FormatC_Install(*section);
	SU2000_Traps_Install(*section,
	                     SU2000_FormatC_Installed(),
	                     SU2000_FormatC_IoBase1(),
	                     SU2000_FormatC_IoBase2());
	SU2000_Net_Install(*section);
	SU2000_Tracker_Install(*section);
	SU2000_Snd_Install(*section);

	const bool installed = SU2000_FormatC_Installed() ||
	                       SU2000_Traps_Installed() ||
	                       SU2000_Net_Installed() ||
	                       SU2000_Tracker_Installed() ||
	                       SU2000_Snd_Installed();
	su2000_active = installed;
	if (installed) {
		// Hardwired SU-2000 NET block (CONFIG1/2.VPC [NET] IO 0x280):
		// the VFX-1 VIP snooper (base 0x280) must stay off ETS's NET
		// probes. Uses the configured base when the NET trap is installed.
		su2000_claim_range(SU2000_Net_IoBase() ? SU2000_Net_IoBase() : 0x280,
		                   32);
	}
}

void SU2000_Destroy()
{
	uninstall();
	su2000_clear_claims();
}

void SU2000_SetEnabled(const bool enabled)
{
	if (enabled) {
		SU2000_Init();
	} else {
		SU2000_Destroy();
	}
}

bool SU2000_IsActive()
{
	return su2000_active;
}
