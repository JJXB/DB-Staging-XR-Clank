// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "xrcfg.h"

#include <string>

#include "utils/checks.h"
#include "utils/string_utils.h"

CHECK_NARROWING();

void XRCFG::Run()
{
	if (HelpRequested()) {
		WriteOut("Manages XR presets (the [xr] settings saved under\n"
		         "xr_ key names).\n"
		         "\n"
		         "Usage:\n"
		         "  XRCFG LIST          List saved presets.\n"
		         "  XRCFG SAVE <name>   Save current [xr] settings.\n"
		         "  XRCFG LOAD <name>   Apply a saved preset now.\n");
		return;
	}
	std::string sub;
	if (!cmd->FindCommand(1, sub)) {
		WriteOut("Missing subcommand. Usage: XRCFG LIST, SAVE <name>, LOAD <name>.\n");
		return;
	}
	upcase(sub);
	if (sub == "LIST") {
		const auto names = OPENXR_ListConfigs();
		if (names.empty()) {
			WriteOut("No XR presets saved yet.\n");
			return;
		}
		for (const auto& name : names) {
			WriteOut("  %s\n", name.c_str());
		}
		return;
	}
	std::string name;
	if (!cmd->FindCommand(2, name)) {
		WriteOut("Missing preset name. Usage: XRCFG %s <name>.\n",
		         sub.c_str());
		return;
	}
	if (sub == "SAVE") {
		if (OPENXR_SaveConfig(name)) {
			WriteOut("Saved XR preset '%s'.\n", name.c_str());
		} else {
			WriteOut("Could not save XR preset '%s'.\n", name.c_str());
		}
		return;
	}
	if (sub == "LOAD") {
		if (OPENXR_LoadConfig(name)) {
			WriteOut("Applied XR preset '%s'.\n", name.c_str());
		} else {
			WriteOut("Could not load XR preset '%s'.\n", name.c_str());
		}
		return;
	}
	WriteOut("Unknown subcommand '%s'. Usage: XRCFG LIST, SAVE <name>, LOAD <name>.\n",
	         sub.c_str());
}
